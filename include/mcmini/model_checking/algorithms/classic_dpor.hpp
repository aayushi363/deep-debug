#pragma once

#include <cstdint>
#include <vector>

#include "mcmini/misc/ddt.hpp"
#include "mcmini/model_checking/algorithm.hpp"
#include "mcmini/model_checking/algorithms/classic_dpor/runner_item.hpp"
#include "mcmini/model_checking/algorithms/classic_dpor/stack_item.hpp"

namespace model_checking {

/**
 * @brief A model-checking algorithm which performs verification using the
 * algorithm of Flanagan and Godefroid (2005).
 */
class classic_dpor final : public algorithm {
public:
  using dependency_relation_type =
      double_dispatch_member_function_table<const model::transition,
                                            bool(void)>;

  using coenabled_relation_type =
      double_dispatch_member_function_table<const model::transition,
                                            bool(void)>;

  void verify_using(coordinator &, const callbacks &) override;
  void verify_using(coordinator &coordinator) {
    callbacks no_callbacks;
    this->verify_using(coordinator, no_callbacks);
  }
  static dependency_relation_type default_dependencies();
  static coenabled_relation_type default_coenabledness();

  struct configuration {
    dependency_relation_type dependency_relation =
        classic_dpor::default_dependencies();
    coenabled_relation_type coenabled_relation =
        classic_dpor::default_coenabledness();
    uint32_t maximum_total_execution_depth = 1500;
    bool assumes_linear_program_flow = false;

    enum class exploration_policy : uint { round_robin, smallest_first };
    exploration_policy policy = exploration_policy::smallest_first;
    bool stop_at_first_deadlock = false;
  };

  classic_dpor() = default;
  classic_dpor(configuration config) : config(std::move(config)) {}

  // --- Fuzzer-side "split DPOR" (libmcmini_intelligence) ---------------------
  // Result of replaying ONE recorded schedule through the model and running the
  // standard classic-DPOR bookkeeping after each step — WITHOUT a live search or
  // backtrack re-execution. This is how the Antithesis fuzzer reuses mcmini's
  // reduction: it drives the model along a schedule the SUT already produced,
  // then reads out where DPOR wants to diverge next.
  struct recorded_analysis {
    bool deadlocked = false;
    uint32_t depth = 0;  // number of transitions executed
    // Indexed by state (0 .. depth). `ran[i]` is the runner whose transition
    // leaves state i (RUNNER_ID_MAX for the final state); `backtrack_sets[i]`
    // and `enabled_sets[i]` are the DPOR backtrack set and enabled set at i.
    std::vector<runner_id_t> ran;
    std::vector<std::vector<runner_id_t>> backtrack_sets;
    std::vector<std::vector<runner_id_t>> enabled_sets;
  };

  // Drive `coord`'s model along `schedule` (runner ids in execution order),
  // running grow_stack_after_running / dynamically_update_backtrack_sets after
  // each step, and return the deadlock flag plus per-state backtrack/enabled
  // sets. `coord` MUST be configured so execute_runner(rid) advances the model
  // to rid's next recorded transition (see libmcmini_intelligence's
  // recorded_process). Reuses the live-search bookkeeping verbatim; performs no
  // backtracking re-execution of its own.
  recorded_analysis analyze_recorded(coordinator &coord,
                                     const std::vector<runner_id_t> &schedule);

private:
  configuration config;

  bool are_dependent(const model::transition &t1,
                     const model::transition &t2) const;

  bool are_coenabled(const model::transition &t1,
                     const model::transition &t2) const;

  bool are_independent(const model::transition &t1,
                       const model::transition &t2) const {
    return !are_dependent(t1, t2);
  }

  // Do not call these methods directly. They are implementation details of
  // the DPOR algorithm and are called at specific points in time!

  struct dpor_context;
  clock_vector accumulate_max_clock_vector_against(const model::transition &,
                                                   const dpor_context &) const;

  void continue_dpor_by_expanding_trace_with(runner_id_t p, dpor_context &);
  void grow_stack_after_running(dpor_context &);
  void dynamically_update_backtrack_sets(dpor_context &);

  bool dynamically_update_backtrack_sets_at_index(
      const dpor_context &, const model::transition &S_i,
      const model::transition &nextSP, stack_item &preSi, size_t i,
      runner_id_t p);
};

} // namespace model_checking
