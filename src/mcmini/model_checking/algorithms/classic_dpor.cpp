#include "mcmini/model_checking/algorithms/classic_dpor.hpp"

#include <sys/types.h>

#include <array>
#include <cassert>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <list>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "mcmini/coordinator/coordinator.hpp"
#include "mcmini/defines.h"
#include "mcmini/log/logger.hpp"
#include "mcmini/model/exception.hpp"
#include "mcmini/model/program.hpp"
#include "mcmini/model/transition.hpp"
#include "mcmini/model/transitions/condition_variables/callbacks.hpp"
#include "mcmini/model/transitions/mutex/callbacks.hpp"
#include "mcmini/model/transitions/mutex/mutex_init.hpp"
#include "mcmini/model/transitions/semaphore/callbacks.hpp"
#include "mcmini/model/transitions/thread/callbacks.hpp"
#include "mcmini/model_checking/algorithms/classic_dpor/clock_vector.hpp"
#include "mcmini/model_checking/algorithms/classic_dpor/runner_item.hpp"
#include "mcmini/model_checking/algorithms/classic_dpor/stack_item.hpp"
#include "mcmini/real_world/process.hpp"

using namespace model;
using namespace logging;
using namespace model_checking;

logger dpor_logger("dpor");

struct classic_dpor::dpor_context {
  ::coordinator &coordinator;
  std::vector<model_checking::stack_item> stack;

  dpor_context(::coordinator &c) : coordinator(c) {}

  const transition *get_transition(int i) const {
    return stack.at(i).get_out_transition();
  }

  bool happens_before(size_t i, size_t j) const {
    const runner_id_t rid = get_transition(i)->get_executor();
    const clock_vector &cv = stack.at(j + 1).get_clock_vector();
    return i <= cv.value_for(rid);
  }

  bool happens_before_thread(size_t i, runner_id_t p) const {
    const runner_id_t rid = get_transition(i)->get_executor();
    const clock_vector &cv =
        stack.back().get_per_runner_clocks()[p].get_clock_vector();
    return i <= cv.value_for(rid);
  }

  bool threads_race_after(size_t i, runner_id_t q, runner_id_t p) const {
    const size_t transition_stack_height = stack.size() - 1;
    for (size_t j = i + 1; j < transition_stack_height; j++) {
      if (q == get_transition(j)->get_executor() &&
          this->happens_before_thread(j, p))
        return true;
    }
    return false;
  }
  std::vector<const transition *> linearize_lowest_first();
  std::vector<const transition *> linearize_greedy() { return {}; }
};

clock_vector classic_dpor::accumulate_max_clock_vector_against(
    const model::transition &t, const dpor_context &context) const {
  // The last state in the stack does NOT have an out transition, hence the
  // `nullptr` check. Note that `s_i.get_out_transition()` refers to `S_i`
  // (case-sensitive) in the paper, viz. the transition between states `s_i` and
  // `s_{i+1}`.
  clock_vector result;
  for (const stack_item &s_i : context.stack) {
    if (s_i.get_out_transition() != nullptr &&
        this->are_dependent(*s_i.get_out_transition(), t)) {
      result = clock_vector::max(result, s_i.get_clock_vector());
    }
  }
  return result;
}

bool classic_dpor::are_coenabled(const model::transition &t1,
                                 const model::transition &t2) const {
  return t1.get_executor() != t2.get_executor() &&
         this->config.coenabled_relation.call_or(true, &t1, &t2);
}

bool classic_dpor::are_dependent(const model::transition &t1,
                                 const model::transition &t2) const {
  return t1.get_executor() == t2.get_executor() ||
         this->config.dependency_relation.call_or(true, &t1, &t2);
}

void classic_dpor::verify_using(coordinator &coordinator,
                                const callbacks &callbacks) {
  // The code below is an implementation of the model-checking algorithm of
  // Flanagan and Godefroid from 2005.

  // 1. Data structure set up

  /// @invariant: The number of items in the DPOR-specific stack is the same
  /// size as the number of transitions in the current trace plus one.
  ///
  /// The initial entry into the stack represents the information DPOR tracks
  /// for state `s_0`.
  log_debug(dpor_logger) << "Initializing the DPOR stack";
  bool reached_max_depth = false;
  std::list<runner_id_t> round_robin_sched;

  stats model_checking_stats;
  dpor_context context(coordinator);
  auto &dpor_stack = context.stack;
  dpor_stack.emplace_back(
      clock_vector(),
      coordinator.get_current_program_model().get_enabled_runners());
  log_very_verbose(dpor_logger)
      << "Initial enabled runners: "
      << coordinator.get_current_program_model().get_enabled_runners();

  while (!dpor_stack.empty()) {
    // 2. Exploration phase
    while (dpor_stack.back().has_enabled_runners()) {
      if (dpor_stack.size() >= this->config.maximum_total_execution_depth) {
        if (!reached_max_depth) {
          log_unexpected(dpor_logger)
              << "*** Execution Limit Reached! ***\n\n"
              << "McMini encountered a trace with" << dpor_stack.size()
              << " transitions which is the most that McMini was configured"
              << " to handle in any given trace ("
              << this->config.maximum_total_execution_depth
              << "). McMini will continue its search, but it may be "
                 "incomplete. Rerun McMini with the "
                 "\"--max-depth-per-thread\" "
              << "flag for correct results.";
          reached_max_depth = true;
        }
        break;
      }
      try {
        runner_id_t rid;
        switch (config.policy) {
        case configuration::exploration_policy::smallest_first: {
          rid = dpor_stack.back().get_first_enabled_runner();
          break;
        }
        case configuration::exploration_policy::round_robin: {
          auto unscheduled_runners = dpor_stack.back().get_enabled_runners();

          // For round robin scheduling, always prefer to schedule
          // threads that don't appear in `round_robin_sched` because
          // these threads are necessarily unscheduled wrt the previous starting
          // point.
          //
          // 1. Among those threads not appearing in the round robin
          // schedule, always pick the smallest.
          // 2. If every enabled thread has already been scheduled, then pick
          // the first based on the round robin schedule and move it to the back
          // of the list. NOTE: Every enabled runner is in `round_robin_sched`,
          // but the converse is not necessarily true (i.e. there may be threads
          // in `round_robin_sched` that aren't enabled)
          for (const runner_id_t id : round_robin_sched) {
            unscheduled_runners.erase(id);
          }
          if (!unscheduled_runners.empty()) {
            rid = *std::min_element(unscheduled_runners.begin(),
                                    unscheduled_runners.end());
            round_robin_sched.push_back(rid);
          } else {
            auto it =
                std::find_if(round_robin_sched.begin(), round_robin_sched.end(),
                             [&](const runner_id_t id) {
                               return dpor_stack.back().is_enabled(id);
                             });
            assert(it != round_robin_sched.end());
            round_robin_sched.splice(round_robin_sched.end(), round_robin_sched,
                                     it);
            rid = *it;
          }
        }
        }

        this->continue_dpor_by_expanding_trace_with(rid, context);
        model_checking_stats.total_transitions++;

        // Now ask the question: will the next operation of this thread
        // cause the program to exit or abort abnormally?
        //
        // If so, stop expanding the trace and backtrack. The rationale is that
        // any extension of this trace will eventually show the same bug anyway
        // (the transition claims to cause the program to abort), and smaller
        // traces are more understandable.
        const transition *rtransition =
            coordinator.get_current_program_model().get_pending_transition_for(
                rid);
        if (rtransition->aborts_program_execution()) {
          throw real_world::process::termination_error(SIGABRT, rid,
                                                       "The program aborted");
        } else if (rtransition->program_exit_code() > 0) {
          throw real_world::process::nonzero_exit_code_error(
              rtransition->program_exit_code(), rid,
              "The program exited abnormally");
        }
      } catch (const model::undefined_behavior_exception &ube) {
        if (callbacks.undefined_behavior)
          callbacks.undefined_behavior(coordinator, model_checking_stats, ube);
        return;
      } catch (const real_world::process::termination_error &te) {
        if (callbacks.abnormal_termination)
          callbacks.abnormal_termination(coordinator, model_checking_stats, te);
        return;
      } catch (const real_world::process::nonzero_exit_code_error &nzec) {
        if (callbacks.nonzero_exit_code)
          callbacks.nonzero_exit_code(coordinator, model_checking_stats, nzec);
        return;
      }
    }

    if (callbacks.trace_completed)
      callbacks.trace_completed(coordinator, model_checking_stats);

    if (config.stop_at_first_deadlock &&
        coordinator.get_current_program_model().is_in_deadlock()) {
      log_info(dpor_logger)
          << "First deadlock found. Reporting and stopping model checking.";
      if (callbacks.deadlock)
        callbacks.deadlock(coordinator, model_checking_stats);
      return;
    } else if (callbacks.deadlock &&
               coordinator.get_current_program_model().is_in_deadlock())
      callbacks.deadlock(coordinator, model_checking_stats);

    // 3. Backtrack phase
    while (!dpor_stack.empty() && dpor_stack.back().backtrack_set_empty())
      dpor_stack.pop_back();

    if (!dpor_stack.empty()) {
      // At this point, the model checker's data structures are valid for
      // `dpor_stack.size()` states; however, the model and the associated
      // process(es) that the model represent do not yet correspond after
      // backtracking.
      log_debug(dpor_logger)
          << "Backtracking to depth `" << (dpor_stack.size() - 1) << "`";
      try {
        coordinator.return_to_depth(dpor_stack.size() - 1);
      } catch (const real_world::process::termination_error &te) {
        // The process spawned to replay history up to this depth (see
        // `coordinator::return_to_depth()`) died before the replay
        // finished. Report it the same way a termination during forward
        // exploration is reported, rather than letting it escape
        // unhandled all the way out of `verify_using()`.
        if (callbacks.abnormal_termination)
          callbacks.abnormal_termination(coordinator, model_checking_stats, te);
        return;
      } catch (const real_world::process::nonzero_exit_code_error &nzec) {
        if (callbacks.nonzero_exit_code)
          callbacks.nonzero_exit_code(coordinator, model_checking_stats, nzec);
        return;
      }
      log_debug(dpor_logger) << "Finished backtracking to depth `"
                             << (dpor_stack.size() - 1) << "`";
      model_checking_stats.trace_id++;

      // The first step of the NEXT exploration phase begins with following
      // one of the backtrack threads. Select one thread to backtrack upon and
      // follow it before continuing onto the exploration phase.
      try {
        this->continue_dpor_by_expanding_trace_with(
            dpor_stack.back().backtrack_set_pop_first(), context);

        // If we're doing round robin scheduling for expanding the trace,
        // backtracking forces a restart of the round robin process.
        // Intuitively, this makes sense: round robin only applies when
        // searching _new_ traces. However, we could be a little more
        // intelligent here and resume the round robin from where it _should_
        // continue, but that's a little more complicated.
        //
        // That is, if the trace after backtracking is e.g.
        //
        // 1, 2, 3, 1, 2, _2_
        //
        // where _2_ was inserted as a backtrack point, then we could notice
        // that _3_ was technically supposed to be next in the round robin
        // scheduling. The current method would schedule 1, then 2, then 3, ...
        // since we only account for scheduling that occurs during _expansion_.
        round_robin_sched.clear();
      } catch (const model::undefined_behavior_exception &ube) {
        if (callbacks.undefined_behavior)
          callbacks.undefined_behavior(coordinator, model_checking_stats, ube);
        return;
      }
    }
  }
}

void classic_dpor::continue_dpor_by_expanding_trace_with(
    runner_id_t p, dpor_context &context) {
  log_debug(dpor_logger) << "DPOR selected `" << p << "` to explore";
  context.coordinator.execute_runner(p);
  log_debug(dpor_logger) << "DPOR expanded following `" << p << "`";
  this->grow_stack_after_running(context);
  this->dynamically_update_backtrack_sets(context);
}

void classic_dpor::grow_stack_after_running(dpor_context &context) {
  // In this method, the following invariants are assumed to hold:
  //
  // 1. `n` := `stack.size()`.
  // 2. `t_n` is the `n`th transition executed in the model of `coordinator`.
  // This transition *has already executed in the model.* THIS IS VERY
  // IMPORTANT as the DPOR information tracking below relies on this heavily.
  //
  // After this method is executed, the stack will have size `n + 1`. Each entry
  // will correspond to the information DPOR cares about for each state in the
  // `coordinator`'s state sequence.
  const coordinator &coordinator = context.coordinator;
  assert(coordinator.get_depth_into_program() == context.stack.size());
  const model::transition *t_n =
      coordinator.get_current_program_model().get_trace().back();

  // NOTE: `cv` corresponds to line 14.3 of figure 4 in the DPOR paper.
  clock_vector cv = accumulate_max_clock_vector_against(*t_n, context);

  // NOTE: The assignment corresponds to line 14.4 of figure 4. Here `S'`
  // represents the transition sequence _after_ `t_n` has executed. Since the
  // `coordinator` already contains this transition (recall the invariants in
  // the comment at the start of this function), `S' ==
  // `coordinator.get_current_program_model().get_trace()` and thus `|S'| ==
  // corodinator.get_depth_into_program().
  cv[t_n->get_executor()] = coordinator.get_depth_into_program();

  // NOTE: This line corresponds to line 14.5 of figure 4. Here, C' is
  // conceptually captured through the DPOR stack and the per-thread DPOR data.
  // The former contains the per-state clock vectors while the latter the
  // per-thread clock vectors (among other data).
  auto s_n_per_runner_clocks = context.stack.back().get_per_runner_clocks();
  s_n_per_runner_clocks[t_n->get_executor()].set_clock_vector(cv);
  context.stack.emplace_back(
      cv, std::move(s_n_per_runner_clocks), t_n,
      coordinator.get_current_program_model().get_enabled_runners());
  stack_item &s_n = context.stack.at(context.stack.size() - 2);
  stack_item &s_n_plus_1 = context.stack.back();

  // INVARIANT: For each thread `p`, if such a thread is contained
  // in the sleep set of `s_n`, then `next(s_n, p)` MUST be the transition
  // that would be contained in that sleep set.
  for (const runner_id_t &rid : s_n.get_sleep_set()) {
    const model::transition *rid_next =
        coordinator.get_current_program_model().get_pending_transition_for(rid);
    if (this->are_independent(*rid_next, *t_n))
      s_n_plus_1.insert_into_sleep_set(rid);
  }

  // `t_n` is inserted into the sleep set AFTER execution. This is how sleep
  // sets work (see papers etc.)
  s_n.set_out_transition(t_n);
  s_n.insert_into_sleep_set(t_n->get_executor());
  s_n.mark_searched(t_n->get_executor());
}

void classic_dpor::dynamically_update_backtrack_sets(dpor_context &context) {
  /*
   * Updating the backtrack sets is accomplished as follows
   * (under the given assumptions)
   *
   * ASSUMPTIONS
   *
   * 1. The state reflects last(S) for the transition stack
   *
   * 2. The thread that ran last is at the top of the transition
   * stack (this should always be true)
   *
   * 3. The next transition for the thread that ran the most
   * recent transition in the transition stack (the transition at the
   * top of the stack) has been properly updated to reflect what that
   * thread will do next
   *
   * WLOG, assume there are `n` transitions in the transition stack
   * and `k` threads that are known to exist at the time of updating
   * the backtrack sets. Note this implies that there are `n+1` items
   * in the state stack (since there is always the initial state + 1
   * for every subsequent transition thereafter)
   *
   * Let
   *  S_i = ith backtracking state item
   *  T_i = ith transition
   *  N_p = the next transition for thread p (next(s, p))
   *
   * ALGORITHM:
   *
   * 1. First, get a reference to the transition at the top
   * of the transition stack (i.e. the most recent transition)
   * as well as the thread that ran that transition. WLOG suppose that
   * thread has a thread id `i`.
   *
   * This transition will be used to test against the transitions
   * queued as running "next" for all of the **other** threads
   * that exist
   *
   *  2. Test whether a backtrack point is needed at state
   *  S_n for the other threads by comparing N_p, for all p != i.
   *
   *  3. Get a reference to N_i and traverse the transition stack
   *  to determine if a backtrack point is needed anywhere for
   *  thread `i`
   */
  const coordinator &coordinator = context.coordinator;
  const size_t num_threads =
      coordinator.get_current_program_model().get_num_runners();

  std::set<runner_id_t> thread_ids;
  for (runner_id_t i = 0; i < num_threads; i++) thread_ids.insert(i);

  const ssize_t t_stack_top = (ssize_t)(context.stack.size()) - 2;
  const runner_id_t last_runner_to_execute =
      coordinator.get_current_program_model()
          .get_trace()
          .back()
          ->get_executor();
  thread_ids.erase(last_runner_to_execute);

  // O(# threads)
  {
    const model::transition &S_n =
        *coordinator.get_current_program_model().get_trace().back();

    for (runner_id_t rid : thread_ids) {
      const model::transition &next_sp =
          *coordinator.get_current_program_model()
               .get_pending_transitions()
               .get_transition_for_runner(rid);
      dynamically_update_backtrack_sets_at_index(context, S_n, next_sp,
                                                 context.stack.at(t_stack_top),
                                                 t_stack_top, rid);
    }
  }

  // O(transition stack size)
  {
    const model::transition &next_s_p_for_latest_runner =
        *coordinator.get_current_program_model()
             .get_pending_transitions()
             .get_transition_for_runner(last_runner_to_execute);

    // It only remains to add backtrack points at the necessary
    // points for thread `last_runner_to_execute`. We start at one step elow the
    // top since we know that transition to not be co-enabled (since it was, by
    // assumption, run by `last_runner_to_execute`)
    for (int i = t_stack_top - 1; i >= 0; i--) {
      const model::transition &S_i =
          *coordinator.get_current_program_model().get_trace().at(i);
      const bool should_stop = dynamically_update_backtrack_sets_at_index(
          context, S_i, next_s_p_for_latest_runner, context.stack.at(i), i,
          last_runner_to_execute);
      /*
       * Stop when we find the _first_ such i; this
       * will be the maxmimum `i` since we're searching
       * backwards
       */
      if (should_stop) break;
    }
  }
}

bool classic_dpor::dynamically_update_backtrack_sets_at_index(
    const dpor_context &context, const model::transition &S_i,
    const model::transition &next_sp, stack_item &pre_si, size_t i,
    runner_id_t p) {
  // TODO: add in co-enabled conditions
  const bool has_reversible_race = this->are_dependent(next_sp, S_i) &&
                                   this->are_coenabled(next_sp, S_i) &&
                                   !context.happens_before_thread(i, p);

  // If there exists i such that ...
  if (has_reversible_race) {
    std::set<runner_id_t> e;

    for (runner_id_t const q : pre_si.get_enabled_runners()) {
      const bool in_e = q == p || context.threads_race_after(i, q, p);

      // If E != empty set
      if (in_e && !pre_si.sleep_set_contains(q))
        e.insert(q);
    }

    if (e.empty()) {
      // E is the empty set -> add every enabled thread at pre(S, i)
      for (runner_id_t const q : pre_si.get_enabled_runners())
        if (!pre_si.sleep_set_contains(q))
          pre_si.insert_into_backtrack_set_unless_completed(q);
    } else {
      for (runner_id_t const q : e) {
        // If there is a thread in preSi that we
        // are already backtracking AND which is contained
        // in the set E, chose that thread to backtrack
        // on. This is equivalent to not having to do
        // anything
        if (pre_si.backtrack_set_contains(q)) return true;
      }

      // Otherwise select an arbitrary thread to backtrack upon.
      pre_si.insert_into_backtrack_set_unless_completed(*e.begin());
    }
  }
  return has_reversible_race;
}

classic_dpor::dependency_relation_type classic_dpor::default_dependencies() {
  classic_dpor::dependency_relation_type dr;
  dr.register_dd_entry<const transitions::thread_create>(
      &transitions::thread_create::depends);
  dr.register_dd_entry<const transitions::thread_join>(
      &transitions::thread_join::depends);
  dr.register_dd_entry<const transitions::thread_start>(
      &transitions::thread_start::depends);
  dr.register_dd_entry<const transitions::thread_exit>(
      &transitions::thread_exit::depends);
  dr.register_dd_entry<const transitions::mutex_lock,
                       const transitions::mutex_init>(
      &transitions::mutex_lock::depends);
  dr.register_dd_entry<const transitions::mutex_lock,
                       const transitions::mutex_lock>(
      &transitions::mutex_lock::depends);
  dr.register_dd_entry<const transitions::condition_variable_wait,
                       const transitions::condition_variable_init>(
      &transitions::condition_variable_wait::depends);
  dr.register_dd_entry<const transitions::condition_variable_wait,
                       const transitions::mutex_lock>(
      &transitions::condition_variable_wait::depends);
  dr.register_dd_entry<const transitions::condition_variable_signal,
                       const transitions::condition_variable_wait>(
      &transitions::condition_variable_signal::depends);
  dr.register_dd_entry<const transitions::condition_variable_signal,
                       const transitions::mutex_lock>(
      &transitions::condition_variable_signal::depends);
  return dr;
}

classic_dpor::coenabled_relation_type classic_dpor::default_coenabledness() {
  classic_dpor::dependency_relation_type cr;
  cr.register_dd_entry<const transitions::thread_create>(
      &transitions::thread_create::coenabled_with);
  cr.register_dd_entry<const transitions::thread_join>(
      &transitions::thread_join::coenabled_with);
  cr.register_dd_entry<const transitions::mutex_lock,
                       const transitions::mutex_unlock>(
      &transitions::mutex_lock::coenabled_with);
  cr.register_dd_entry<const transitions::condition_variable_signal,
                       const transitions::condition_variable_wait>(
      &transitions::condition_variable_signal::coenabled_with);
  cr.register_dd_entry<const transitions::condition_variable_signal,
                       const transitions::mutex_unlock>(
      &transitions::condition_variable_signal::coenabled_with);
  cr.register_dd_entry<const transitions::condition_variable_broadcast,
                       const transitions::condition_variable_wait>(
      &transitions::condition_variable_broadcast::coenabled_with);
  cr.register_dd_entry<const transitions::condition_variable_broadcast,
                       const transitions::mutex_unlock>(
      &transitions::condition_variable_broadcast::coenabled_with);
  cr.register_dd_entry<const transitions::condition_variable_destroy,
                       const transitions::condition_variable_wait>(
      &transitions::condition_variable_destroy::coenabled_with);
  cr.register_dd_entry<const transitions::condition_variable_destroy,
                       const transitions::condition_variable_signal>(
      &transitions::condition_variable_destroy::coenabled_with);
  return cr;
}

std::vector<const transition *>
classic_dpor::dpor_context::linearize_lowest_first() {
  if (this->stack.empty())
    return {};

  // N = number of _transitions_ in the stack
  // Each element is one _state_, so there's one
  // less transition
  const size_t N = this->stack.size() - 1;

  // --- Phase 1 ---
  // Dependency Graph Construction
  //
  //
  // `adj_list` holds the dependencies between every point in the trace. Nodes
  // are representing using integers and correspond to the index in the
  // transition stack.
  //
  // `e in adj_list[i] <--> happens_before(i, e)`.
  //
  // NOTE: For each `i`, the nodes are ordered in topological order. This is
  // because the transitions in the stack are already topologically ordered by
  // construction, and we process nodes in order.
  std::vector<std::vector<int>> adj_list(N);
  std::vector<std::vector<bool>> redundant(N);

  for (size_t i = 0; i < N; i++) {
    for (size_t j = i + 1; j < N; j++) {
      if (happens_before(i, j)) {
        adj_list[i].push_back(j);
        redundant[i].push_back(true);
      }
    }
  }

  // --- Phase 2 ---
  // Compute Chain Decomposition
  //
  // Decomposes the DAG into a minimum number of disjoint paths (chains).
  std::vector<bool> unmarked(N, true);
  std::vector<int> chain_id(N);  // chain_id[v]: index of the chain containing v
  std::vector<std::list<int>> C; // C[h]: list of nodes in chain h

  // Iterating in increasing topological order (see note above)
  for (size_t v = 0; v < N; ++v) {
    if (unmarked[v]) {
      C.emplace_back(); // Start a new chain
      int current_chain_id = C.size() - 1;
      int current_node = v;

      while (current_node != -1) {
        C.back().push_back(current_node);
        chain_id[current_node] = current_chain_id;
        unmarked[current_node] = false;

        int next_node = -1;
        // Find the first unmarked neighbor 'w' (smallest topological rank)
        for (int w : adj_list[current_node]) {
          if (unmarked[w]) {
            next_node = w;
            break;
          }
        }
        current_node = next_node;
      }
    }
  }

  // --- Phase 2: Compute Reachability and Redundancy ---
  // reach[i][c]: min{j ; node j in C[c] and path i -> j exists}
  // where "<" refers to topological order
  //
  // Intuitively, `reach[i][c]` denotes the "smallest" (topologically-speaking)
  // node in chain `c` that has a path to node `i`.
  const int num_chains = C.size();
  const int INF = N + 1;
  std::vector<std::vector<int>> reach(N, std::vector<int>(num_chains, INF));

  // The key loop: iterates in DECREASING topological order
  for (size_t k = 0; k <= N - 1; k++) {
    size_t v = N - k - 1;
    auto &reach_v = reach[v];

    // Process outgoing edges of v (target 'w' is in INCREASING order)
    for (int w : adj_list[v]) {
      if (w < reach_v[chain_id[w]]) {
        redundant[v][w] = false;

        // Update reach[i] using reach[j]:
        // If v can reach w via a non-redundant edge, v can reach everything j
        // reaches.
        for (int c = 0; c < num_chains; ++c)
          reach_v[c] = std::min(reach_v[c], reach[w][c]);
      }
    }
    reach_v[chain_id[v]] = v;
  }

  // --- Phase 3 ---
  // Remove redundant edges from the adjacency list
  for (size_t k = 0; k < N; k++) {
    auto &out_edges = adj_list[k];
    auto &redundant_edges = redundant[k];
    size_t j = 0;
    for (size_t i = 0; i < out_edges.size(); ++i)
      if (redundant_edges[i])
        out_edges[j++] = out_edges[i];
    out_edges.resize(j);
  }

  // --- Phase 4 ---
  // Topological Sort and Final Processing
  // Generate a topological sort using the newly reduced dependency graph
  std::vector<int> linearization;
  std::vector<int> ready_set; // Indicies into the transition stack
  std::vector<int> in_degree(N, 0);
  for (size_t u = 0; u < N; ++u)
    for (int v : adj_list[u])
      in_degree[v]++;

  for (size_t i = 0; i < N; ++i)
    if (in_degree[i] == 0)
      ready_set.push_back(i);

  runner_id_t last_rid = -1; // -1 indicates no last rid
  while (!ready_set.empty()) {
    int next_vertex = -1;
    int selected_index = -1;

    // Find the vertex with the same executor as the last one
    if (last_rid != -1) {
      for (size_t i = 0; i < ready_set.size(); ++i) {
        const runner_id_t r =
            stack.at(ready_set[i]).get_out_transition()->get_executor();
        if (r == last_rid) {
          next_vertex = ready_set[i];
          selected_index = i;
          break; // Found the best greedy match that avoids inversion
        }
      }
    }

    // If no color match, or at the start, pick the thread with the lowest id
    if (next_vertex == -1) {
      // INVARIANT: Since mid_rid
      runner_id_t min_rid = RID_INVALID;
      for (size_t i = 0; i < ready_set.size(); ++i) {
        const runner_id_t r =
            stack.at(ready_set[i]).get_out_transition()->get_executor();
        if (r < min_rid) {
          next_vertex = ready_set[i];
          selected_index = i;
        }
      }
    }

    linearization.push_back(next_vertex);
    last_rid = stack.at(next_vertex).get_out_transition()->get_executor();

    // Remove the selected vertex from the ready set
    // Swap with the back and pop for O(1) removal from vector (since order in S
    // doesn't matter)
    std::swap(ready_set[selected_index], ready_set.back());
    ready_set.pop_back();

    for (int v : adj_list[next_vertex]) {
      in_degree[v]--;
      if (in_degree[v] == 0) {
        ready_set.push_back(v);
      }
    }
  }

  // --- Phase 5 ---
  // Convert the linearization into a transition trace
  std::vector<const transition *> linearized_trace(N);
  for (size_t i = 0; i < N; i++)
    linearized_trace[i] = stack.at(linearization[i]).get_out_transition();
  return linearized_trace;
}