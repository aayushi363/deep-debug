// Deadlock detector using SIGALRM and CLOCK_PROCESS_CPUTIME_ID
#ifndef DEADLOCK_DETECTOR_H
#define DEADLOCK_DETECTOR_H

#include <stdbool.h>

// Install the deadlock detector timer and handler. If `enable` is false,
// the detector is uninstalled.
void mc_install_deadlock_detector(bool enable);

// Thread lifetime — call from mc_register_this_thread (RECORD mode) and the
// thread-exit path in mc_thread_routine_wrapper.
void dd_thread_start(void);
void dd_thread_exit(void);

// Blocking-wait markers — call from wrapper slow paths (mutex contended,
// etc.).  dd_enter_block checks whether all active threads are now blocked
// and aborts with a deadlock report if so.
void dd_enter_block(void);
void dd_exit_block(void);
// Call from timedlock retry loop after sustained blocking (not on first entry).
void dd_check_deadlock(void);

// Legacy progress hook — kept for callers that still want to poke the
// liveness sampler (barrier, cond, join wrappers).  No longer does the
// expensive CLOCK_PROCESS_CPUTIME_ID syscall.
void deadlock_detector_increment_progress(void);

#endif // DEADLOCK_DETECTOR_H
