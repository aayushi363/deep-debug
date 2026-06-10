#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mcmini/defines.h"  // runner_id_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file lockset.h
 * @brief Phase-1 ("record") Eraser-style lockset race *predictor*.
 *
 * This is a cheap, online, intentionally *unsound* detector that runs while the
 * target executes natively during the RECORD phase. Its only job is to flag
 * *plausible* data races so Phase 1 knows where to pin a checkpoint and stop;
 * the (sound) DPOR model checker in Phase 2 confirms whether a flagged race is
 * real. False positives are therefore acceptable by design.
 *
 * Algorithm (Savage et al., "Eraser"): for every shared memory location we
 * track the set of locks held on *every* access to it (the candidate lockset,
 * refined by intersection). If that set ever becomes empty once the location is
 * shared and written by more than one thread, the lock discipline has been
 * violated and we report a candidate race. A per-location state machine
 * (virgin -> exclusive -> shared -> shared-modified) suppresses the common
 * false positives from single-threaded initialization and read-only sharing.
 *
 * Inputs are reused from machinery McMini already has:
 *   - lock/unlock events come from the mutex wrappers (lockset_acquire/release),
 *   - memory accesses come from the LLVM instrumentation hooks (lockset_on_access).
 *
 * Thread-safety: held-lock sets are thread-local (no synchronization). The
 * per-location shadow map and the lock-bit registry are guarded internally and
 * use the *real* libpthread primitives so they never recurse into McMini's own
 * wrappers.
 *
 * Granularity / limits (v1): accesses are keyed at word (8-byte) granularity by
 * their start address; at most 64 distinct locks are tracked (locks beyond that
 * are treated as non-protecting, which can only add false positives). Both are
 * acceptable for a predictor and can be widened later.
 */

/// @brief Initialize the predictor. Idempotent. Reads `MCMINI_LOCKSET` from the
/// environment to decide whether the predictor is enabled for this run.
void lockset_init(void);

/// @brief Whether Phase-1 race prediction is enabled (i.e. `MCMINI_LOCKSET` set).
int lockset_is_enabled(void);

/// @brief Record that the calling thread acquired @p mutex (RECORD phase).
/// Adds @p mutex to the calling thread's thread-local held-lock set.
void lockset_acquire(void *mutex);

/// @brief Record that the calling thread released @p mutex (RECORD phase).
/// Removes @p mutex from the calling thread's thread-local held-lock set.
void lockset_release(void *mutex);

/// @brief Clear the calling thread's held-lock set (e.g. at thread start).
void lockset_thread_reset(void);

/// @brief Feed one instrumented memory access into the predictor (RECORD phase).
/// @param self     runner id of the accessing thread (e.g. `tid_self`)
/// @param addr     accessed address
/// @param size     access width in bytes
/// @param site_id  LLVM-pass site id identifying the source location
/// @param is_write non-zero for a write, zero for a read
void lockset_on_access(runner_id_t self, void *addr, size_t size,
                       uintptr_t site_id, int is_write);

/// @brief Handler invoked once per distinct racing (site_a, site_b) pair.
typedef void (*lockset_race_handler)(void *addr, uintptr_t site_a, int write_a,
                                     runner_id_t thread_a, uintptr_t site_b,
                                     int write_b, runner_id_t thread_b);

/// @brief Override the race handler. If unset, a default handler logs to stderr.
void lockset_set_race_handler(lockset_race_handler handler);

#ifdef __cplusplus
}
#endif
