/*
 * mcmini_intelligence.h — C ABI for the fuzzer-side "split DPOR".
 *
 * This is the interface the Antithesis fuzzer (fuzzer/plugin-rs) binds to in
 * order to reuse mcmini's model + classic DPOR WITHOUT a live process. The
 * implementation (src/mcmini/intelligence/mcmini_intelligence.cpp) replays a
 * recorded schedule through mcmini's model via a `recorded_process`, runs the
 * standard classic-DPOR bookkeeping (classic_dpor::analyze_recorded), and hands
 * back deadlock detection + the per-state backtrack sets that tell the fuzzer
 * where a real DPOR search would diverge next.
 *
 * Design: opaque handle, narrow interface, out-buffer + capacity pattern for
 * variable-sized reads, single-threaded caller ownership.
 */
#ifndef MCMINI_INTELLIGENCE_H
#define MCMINI_INTELLIGENCE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Operation codes — must match `transition_type` in
 * mcmini/spy/checkpointing/transitions.h. */
typedef enum {
  MC_OP_MUTEX_INIT = 1,
  MC_OP_MUTEX_LOCK = 2,
  MC_OP_MUTEX_UNLOCK = 3,
  MC_OP_THREAD_CREATE = 4,
  MC_OP_THREAD_JOIN = 5,
  MC_OP_THREAD_EXIT = 6,
} mc_op_type_t;

/**
 * One transition a runner executes, in the order the model discovers it — i.e.
 * this is the runner's NEXT operation (mcmini's model applies a runner's current
 * pending transition and installs this one as the new pending).
 *
 * `payload` is the operation's object token:
 *   - mutex ops:      the mutex address (any stable per-mutex token).
 *   - THREAD_CREATE:  a unique token for the child (distinct per child; the
 *                     model assigns the child a runner id in creation order).
 *   - THREAD_JOIN:    the target's MODEL runner id.
 *   - THREAD_EXIT:    ignored.
 */
typedef struct {
  uint32_t op;
  uint64_t payload;
} mc_op_t;

/** Opaque result of one recorded-schedule analysis. Free with
 * mc_intel_result_free. */
typedef struct mc_intel_result mc_intel_result_t;

/**
 * Replay `schedule` (runner ids in execution order, beginning with main's
 * implicit thread_start) through mcmini's model + classic DPOR.
 *
 * `runner_ops[r]` is runner r's NEXT-op sequence (length `runner_nops[r]`):
 * the k-th time the schedule steps runner r, the model is advanced to
 * `runner_ops[r][k]`. `n_runners` is the number of runners (main is 0).
 *
 * @return an opaque result handle (caller frees), or NULL on error (e.g. the
 *         schedule drives a disabled transition).
 */
mc_intel_result_t *mc_intel_analyze_recorded(uint32_t n_runners,
                                             const mc_op_t *const *runner_ops,
                                             const uint32_t *runner_nops,
                                             const uint32_t *schedule,
                                             uint32_t schedule_len);

/** @return 1 if the replayed schedule ends in a deadlock, 0 otherwise. */
int mc_intel_is_deadlocked(const mc_intel_result_t *);

/** @return the number of transitions executed. */
uint32_t mc_intel_depth(const mc_intel_result_t *);

/** @return the number of states (depth + 1). */
uint32_t mc_intel_num_states(const mc_intel_result_t *);

/** @return the runner that ran out of state `state` (UINT32_MAX for the final
 * state or out-of-range). */
uint32_t mc_intel_ran_at(const mc_intel_result_t *, uint32_t state);

/**
 * Fill `out_buf` (capacity `cap`) with the DPOR backtrack set at `state` — the
 * runners a real search should also try from that state. Returns the total
 * count (may exceed `cap`; retry with a larger buffer).
 */
size_t mc_intel_backtrack_at(const mc_intel_result_t *, uint32_t state,
                             uint32_t *out_buf, size_t cap);

void mc_intel_result_free(mc_intel_result_t *);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCMINI_INTELLIGENCE_H */
