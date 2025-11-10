#define _GNU_SOURCE
#include "deadlock_detector.h"
#include "mcmini/spy/intercept/interception.h"
#include "mcmini/wrapper_timing.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>

// Thresholds and counters
static struct timespec prev_cpu_time = {0, 0};
static atomic_int quiet_intervals = 0;
// Number of consecutive low-CPU intervals before we declare deadlock
// Use the user's requested N = 100.
static const int QUIET_THRESHOLD = 100;
// The minimum CPU time (in nanoseconds) that counts as "progress"
// If less than this between ticks, we consider that "no CPU progress".
static const long PROGRESS_NSEC = 5000000; // 5 ms
/* Livelock detection parameters */
/* If no progress for this many samples (~10ms per sample), consider it stalled */
static const long PROG_NO_ADVANCE_SAMPLES = 1000; /* ~10s - generous timeout for cond_wait scenarios */
/* If CPU-time advanced by more than this while no progress, treat as livelock */
static const unsigned long CPU_BUSY_THRESHOLD_NS = 1000000000UL; /* 1 second - very generous to account for cond_wait timeout loops */
// Occasional tick counter for debug printing
static atomic_long tick_count = 0;
// Progress counter: wrappers may still increment this (kept for compatibility),
// but detection is now CPU-time based.
static atomic_ulong progress_counter = 0;

/* Track last visible progress (updated in deadlock_detector_increment_progress)
 * We store the sampler tick at which progress last occurred and the
 * process CPU-time (in ns) at that moment. The sampler uses these to
 * detect livelock: long absence of visible progress while CPU-time
 * continues to advance.
 */
static atomic_long last_progress_tick = ATOMIC_VAR_INIT(0);
static atomic_ulong last_progress_cpu_ns = ATOMIC_VAR_INIT(0);

/* Monitor thread removed: we keep detection logic in the sampler thread
 * only to avoid creating extra threads that would be recorded in Phase I.
 */

/* Sampler thread: periodically (every 10ms real time) sample
 * CLOCK_PROCESS_CPUTIME_ID to measure process CPU-time progress.
 * This avoids using ITIMER_REAL/SIGALRM which other libraries may
 * clear (via alarm(0)).
 */
static pthread_t sampler_thread;
static atomic_bool sampler_running = ATOMIC_VAR_INIT(false);

static void *deadlock_detector_sampler_thread(void *arg) {
  struct timespec sleep_ts = {0, 10000000L}; /* 10ms */
  while (atomic_load(&sampler_running)) {
    struct timespec current_cpu_time;
    if (syscall(SYS_clock_gettime, CLOCK_PROCESS_CPUTIME_ID, &current_cpu_time) == -1) {
      /* If sampling fails, just sleep and retry. */
      nanosleep(&sleep_ts, NULL);
      continue;
    }

    if (prev_cpu_time.tv_sec == 0 && prev_cpu_time.tv_nsec == 0) {
      prev_cpu_time = current_cpu_time;
      nanosleep(&sleep_ts, NULL);
      continue;
    }

    long diff_sec = current_cpu_time.tv_sec - prev_cpu_time.tv_sec;
    long diff_nsec = current_cpu_time.tv_nsec - prev_cpu_time.tv_nsec;
    if (diff_nsec < 0) {
      diff_nsec += 1000000000L;
      diff_sec--;
    }
    long total_nsec = diff_sec * 1000000000L + diff_nsec;

    long t = atomic_fetch_add(&tick_count, 1) + 1;
    /* No periodic debug output (keep output minimal). */

    if (total_nsec < PROGRESS_NSEC) {
      int q = atomic_fetch_add(&quiet_intervals, 1) + 1;
      if (q >= QUIET_THRESHOLD) {
        /* Attempt to save timing report directly from the sampler thread.
         * The sampler thread is created with the real pthread_create via
         * libpthread_pthread_create, so it won't be recorded by mc_pthread_create
         * and it's safe to call the non-async-safe save routine here.
         */
        save_timing_report(NULL);
        _exit(1);
      }
    } else {
      atomic_store(&quiet_intervals, 0);
    }

    /* Livelock detection: if we've had no visible progress for a long time
     * (measured in samples) while process CPU-time has advanced by a
     * significant amount, treat this as a livelock and abort. */
    {
      long last_prog_tick = atomic_load(&last_progress_tick);
      long samples_since_prog = (t > last_prog_tick) ? (t - last_prog_tick) : 0;
      if (samples_since_prog >= PROG_NO_ADVANCE_SAMPLES) {
        unsigned long curr_cpu_ns = (unsigned long)current_cpu_time.tv_sec * 1000000000UL +
                                   (unsigned long)current_cpu_time.tv_nsec;
        unsigned long last_cpu_ns = atomic_load(&last_progress_cpu_ns);
        unsigned long cpu_advance = (curr_cpu_ns > last_cpu_ns) ? (curr_cpu_ns - last_cpu_ns) : 0UL;
        if (cpu_advance >= CPU_BUSY_THRESHOLD_NS) {
          /* Save timing report directly from the sampler thread, then exit. */
          save_timing_report(NULL);
          _exit(1);
        }
      }
    }

    prev_cpu_time = current_cpu_time;
    nanosleep(&sleep_ts, NULL);
  }
  return NULL;
}

void mc_install_deadlock_detector(bool enable) {
  static bool installed = false;
  if (enable && !installed) {
  installed = true;

    /* Start sampler thread. Use the real libpthread pthread_create so
     * the mc_pthread_create wrapper does not record these internal
     * detector threads into the Phase I recording. This prevents the
     * detector's helper threads from being modeled in Phase II.
     */
    if (!atomic_load(&sampler_running)) {
      atomic_store(&sampler_running, true);
      /* libpthread_pthread_create is provided by our interception layer
       * and forwards to the real pthread_create (bypassing mc_pthread_create).
       */
      libpthread_pthread_create(&sampler_thread, NULL, deadlock_detector_sampler_thread, NULL);
      pthread_detach(sampler_thread);
    }

    /* No monitor thread: sampler thread performs detection and is silent.
     * This keeps Phase I recording minimal.
     */
  } else if (!enable && installed) {
    /* Stop sampler */
    atomic_store(&sampler_running, false);
    installed = false;
  }
}


void deadlock_detector_increment_progress(void) {
  atomic_fetch_add(&progress_counter, 1);
  /* Record the tick and current process-CPU time for livelock detection. */
  long cur_tick = atomic_load(&tick_count);
  atomic_store(&last_progress_tick, cur_tick);
  struct timespec now;
  if (syscall(SYS_clock_gettime, CLOCK_PROCESS_CPUTIME_ID, &now) == 0) {
    unsigned long ns = (unsigned long)now.tv_sec * 1000000000UL + (unsigned long)now.tv_nsec;
    atomic_store(&last_progress_cpu_ns, ns);
  }
}
