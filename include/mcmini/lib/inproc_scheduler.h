#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// In-SUT runner_mailbox scheduler for the clean-split FUZZER_STANDALONE mode.
//
// Called once from libmcmini_main() when MCMINI_FUZZER_STANDALONE is set. It:
//   * allocates the mailbox array in PROCESS memory (no shm_open — the clean
//     split has no separate coordinator process),
//   * registers the main thread (runner 0),
//   * spawns a dedicated scheduler thread inside the SUT.
//
// Thereafter every mutex/thread wrapper (FUZZER_STANDALONE) parks in its
// mailbox; the scheduler thread emits the enabled set, calls fuzz_getchar() to
// pick which enabled runner to release (idx = byte % enabled_count), releases
// exactly that runner, and detects deadlock (enabled set empty). NO DPOR lives
// here — this is a mechanism; the DPOR brain is on the fuzzer side.
void inproc_scheduler_init(void);

#ifdef __cplusplus
}
#endif
