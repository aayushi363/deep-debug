// Deadlock detector using SIGALRM and CLOCK_PROCESS_CPUTIME_ID
#ifndef DEADLOCK_DETECTOR_H
#define DEADLOCK_DETECTOR_H

#include <stdbool.h>

// Install the deadlock detector timer and handler. If `enable` is false,
// the detector is uninstalled.
void mc_install_deadlock_detector(bool enable);
// Increment the progress counter used by the detector. Call this from
// wrapper functions when a visible progress event occurs (e.g., lock
// acquired, unlock, or other visible operation).
void deadlock_detector_increment_progress(void);

// Read the current progress counter value (count of recorded pthread
// operations since the detector was installed). Used by the schedule-stub
// emitter as the `total_transitions` stat.
unsigned long deadlock_detector_get_progress(void);

#endif // DEADLOCK_DETECTOR_H
