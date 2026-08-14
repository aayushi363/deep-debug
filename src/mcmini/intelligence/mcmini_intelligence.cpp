// mcmini_intelligence.cpp — implementation of the fuzzer-side "split DPOR" C ABI
// (see include/mcmini/intelligence/mcmini_intelligence.h).
//
// Reuses mcmini's model + classic DPOR unchanged: a `recorded_process` stands in
// for the live SUT so the coordinator -> transition_registry callback ->
// program::model_execution_of -> grow_stack -> update_backtrack pipeline runs
// with no DMTCP. classic_dpor::analyze_recorded drives the recorded schedule and
// returns the deadlock flag + per-state backtrack sets.

#include "mcmini/intelligence/mcmini_intelligence.h"

#include <pthread.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <vector>

#include "mcmini/coordinator/coordinator.hpp"
#include "mcmini/model/program.hpp"
#include "mcmini/model/transition_registry.hpp"
#include "mcmini/model_checking/algorithms/classic_dpor.hpp"
#include "mcmini/real_world/mailbox/runner_mailbox.h"
#include "mcmini/real_world/process.hpp"
#include "mcmini/real_world/process_source.hpp"
#include "mcmini/spy/checkpointing/transitions.h"

namespace {

// A stand-in for real_world::process backed by recorded per-runner op queues.
// execute_runner(rid) hands back a synthesized runner_mailbox describing rid's
// NEXT recorded operation; the coordinator's registered callback parses it and
// updates the model exactly as it would from a live process.
struct recorded_process : public real_world::process {
  std::vector<std::deque<mc_op_t>> queues;
  runner_mailbox mb;

  explicit recorded_process(uint32_t n_runners) : queues(n_runners) {
    memset(&mb, 0, sizeof mb);
  }

  pid_t get_pid() const override { return 0; }

  volatile runner_mailbox *execute_runner(runner_id_t rid) override {
    if (rid >= queues.size() || queues[rid].empty())
      throw execution_error("recorded_process: no recorded op for runner");
    const mc_op_t op = queues[rid].front();
    queues[rid].pop_front();
    mb.type = op.op;
    memset((void *)mb.cnts, 0, sizeof mb.cnts);
    switch (op.op) {
      case MC_OP_MUTEX_INIT:
      case MC_OP_MUTEX_LOCK:
      case MC_OP_MUTEX_UNLOCK: {
        void *ptr = (void *)(uintptr_t)op.payload;
        memcpy((void *)mb.cnts, &ptr, sizeof ptr);
        break;
      }
      case MC_OP_THREAD_CREATE: {
        pthread_t t = (pthread_t)op.payload;
        memcpy((void *)mb.cnts, &t, sizeof t);
        break;
      }
      case MC_OP_THREAD_JOIN: {
        runner_id_t tgt = (runner_id_t)op.payload;
        memcpy((void *)mb.cnts, &tgt, sizeof tgt);
        break;
      }
      default:
        break;  // thread_start / thread_exit carry no payload
    }
    return &mb;
  }
};

struct recorded_process_source : public real_world::process_source {
  std::unique_ptr<recorded_process> proc;
  explicit recorded_process_source(std::unique_ptr<recorded_process> p)
      : proc(std::move(p)) {}
  std::unique_ptr<real_world::process> make_new_process() override {
    return std::move(proc);  // only made once (analyze_recorded never re-execs)
  }
};

}  // namespace

struct mc_intel_result {
  model_checking::classic_dpor::recorded_analysis analysis;
};

extern "C" mc_intel_result_t *mc_intel_analyze_recorded(
    uint32_t n_runners, const mc_op_t *const *runner_ops,
    const uint32_t *runner_nops, const uint32_t *schedule,
    uint32_t schedule_len) {
  try {
    std::unique_ptr<recorded_process> proc(new recorded_process(n_runners));
    for (uint32_t r = 0; r < n_runners; r++)
      for (uint32_t i = 0; i < runner_nops[r]; i++)
        proc->queues[r].push_back(runner_ops[r][i]);

    coordinator coord(
        model::program::starting_from_main(),
        model::transition_registry::default_registry(),
        std::unique_ptr<real_world::process_source>(
            new recorded_process_source(std::move(proc))));

    std::vector<runner_id_t> sched;
    sched.reserve(schedule_len);
    for (uint32_t i = 0; i < schedule_len; i++)
      sched.push_back(static_cast<runner_id_t>(schedule[i]));

    model_checking::classic_dpor algo;
    return new mc_intel_result{algo.analyze_recorded(coord, sched)};
  } catch (const std::exception &e) {
    fprintf(stderr, "[mcmini_intelligence] analyze_recorded failed: %s\n",
            e.what());
    return nullptr;
  }
}

extern "C" int mc_intel_is_deadlocked(const mc_intel_result_t *r) {
  return (r && r->analysis.deadlocked) ? 1 : 0;
}

extern "C" uint32_t mc_intel_depth(const mc_intel_result_t *r) {
  return r ? r->analysis.depth : 0;
}

extern "C" uint32_t mc_intel_num_states(const mc_intel_result_t *r) {
  return r ? static_cast<uint32_t>(r->analysis.backtrack_sets.size()) : 0;
}

extern "C" uint32_t mc_intel_ran_at(const mc_intel_result_t *r, uint32_t state) {
  if (!r || state >= r->analysis.ran.size()) return UINT32_MAX;
  return static_cast<uint32_t>(r->analysis.ran[state]);
}

extern "C" size_t mc_intel_backtrack_at(const mc_intel_result_t *r,
                                        uint32_t state, uint32_t *out_buf,
                                        size_t cap) {
  if (!r || state >= r->analysis.backtrack_sets.size()) return 0;
  const auto &bs = r->analysis.backtrack_sets[state];
  for (size_t i = 0; i < bs.size() && i < cap; i++)
    out_buf[i] = static_cast<uint32_t>(bs[i]);
  return bs.size();
}

extern "C" void mc_intel_result_free(mc_intel_result_t *r) { delete r; }
