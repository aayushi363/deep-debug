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

/* ── blocked-thread-cycle detection ─────────────────────────────────────
 *
 * Two atomic counters track thread state in RECORD mode:
 *
 *   g_active_threads  – threads that have registered and not yet exited
 *   g_blocked_threads – threads currently inside a blocking-wait slow path
 *                       (e.g. contended mutex_lock timedlock loop)
 *
 * When g_blocked_threads reaches g_active_threads every live thread is
 * waiting for a resource held by another waiting thread — a deadlock.
 * Detection is O(1) per operation: one atomic increment + one load.
 */
static atomic_int g_active_threads  = ATOMIC_VAR_INIT(0);
static atomic_int g_blocked_threads = ATOMIC_VAR_INIT(0);

void dd_thread_start(void) {
    atomic_fetch_add(&g_active_threads, 1);
}

void dd_thread_exit(void) {
    atomic_fetch_sub(&g_active_threads, 1);
}

void dd_enter_block(void) {
    atomic_fetch_add(&g_blocked_threads, 1);
    /* Deadlock check is intentionally NOT done here to avoid false positives
     * from transient contention: multiple threads can transiently all wait for
     * the same uncontended mutex (one is releasing it) without a cycle existing.
     * The check is deferred to dd_check_deadlock(), called from the timedlock
     * retry loop after 1 second of sustained blocking. */
}

void dd_exit_block(void) {
    atomic_fetch_sub(&g_blocked_threads, 1);
}

/* Called from the timedlock retry loop after sustained waiting.
 * Aborts only when all active threads remain simultaneously blocked after
 * a 100 ms confirmation window.  The extra sleep closes a false-positive race:
 * a holder that releases its mutex and immediately calls dd_thread_exit() drops
 * g_active before the waiting threads (still mid-sleep inside timedlock) can
 * decrement g_blocked.  Within 100 ms (> one 50 ms timedlock interval) any
 * waiter that was unblocked by the release will have acquired the mutex and
 * called dd_exit_block().  A genuine deadlock cycle never resolves. */
void dd_check_deadlock(void) {
    int blocked = atomic_load(&g_blocked_threads);
    int active  = atomic_load(&g_active_threads);
    if (active > 0 && blocked >= active) {
        struct timespec confirm_ts = {0, 100000000L}; /* 100 ms */
        nanosleep(&confirm_ts, NULL);
        blocked = atomic_load(&g_blocked_threads);
        active  = atomic_load(&g_active_threads);
        if (active > 0 && blocked >= active) {
            const char msg[] = "Deadlock detected: all threads blocked\n";
            write(STDERR_FILENO, msg, sizeof(msg) - 1);
            _exit(1);
        }
    }
}

/* ── CPU-idle liveness sampler ───────────────────────────────────────────
 *
 * Catches liveness violations that are not mutex cycles: e.g. a thread
 * sleeping forever in cond_timedwait because the signal was never sent.
 * When total process CPU usage drops below PROGRESS_NSEC for
 * QUIET_THRESHOLD consecutive 10 ms samples (~1 second) we assume the
 * program is permanently stuck and abort.
 *
 * This sampler runs every 10 ms, so its single CLOCK_PROCESS_CPUTIME_ID
 * syscall per interval (100 calls/s) is negligible compared to the
 * millions of per-mutex-operation calls that the old
 * deadlock_detector_increment_progress() used to make.
 */
static struct timespec prev_cpu_time = {0, 0};
static atomic_int quiet_intervals = 0;
// Number of consecutive low-CPU intervals before we declare deadlock
// Use the user's requested N = 100.
static const int QUIET_THRESHOLD = 100;
// The minimum CPU time (in nanoseconds) that counts as "progress"
// If less than this between ticks, we consider that "no CPU progress".
static const long PROGRESS_NSEC = 5000000; // 5 ms
static atomic_long tick_count     = ATOMIC_VAR_INIT(0);
static atomic_ulong progress_counter = ATOMIC_VAR_INIT(0);

static pthread_t sampler_thread;
static atomic_bool sampler_running = ATOMIC_VAR_INIT(false);

static void *deadlock_detector_sampler_thread(void *arg) {
  struct timespec sleep_ts = {0, 10000000L}; /* 10 ms */
  while (atomic_load(&sampler_running)) {
    struct timespec current_cpu_time;
    if (syscall(SYS_clock_gettime, CLOCK_PROCESS_CPUTIME_ID,
                &current_cpu_time) == -1) {
      nanosleep(&sleep_ts, NULL);
      continue;
    }

    if (prev_cpu_time.tv_sec == 0 && prev_cpu_time.tv_nsec == 0) {
      prev_cpu_time = current_cpu_time;
      nanosleep(&sleep_ts, NULL);
      continue;
    }

    long diff_nsec = (current_cpu_time.tv_sec - prev_cpu_time.tv_sec)
                         * 1000000000L
                     + (current_cpu_time.tv_nsec - prev_cpu_time.tv_nsec);

    atomic_fetch_add(&tick_count, 1);

    if (diff_nsec < PROGRESS_NSEC) {
      if (atomic_fetch_add(&quiet_intervals, 1) + 1 >= QUIET_THRESHOLD) {
        save_timing_report(NULL);
        _exit(1);
      }
    } else {
      atomic_store(&quiet_intervals, 0);
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
    /* Kept for API compatibility (barrier, cond, join wrappers still call it).
     * The old CLOCK_PROCESS_CPUTIME_ID syscall has been removed — at 24M
     * calls/timestep for large N it was the dominant recording overhead.
     * The CPU-idle sampler above handles liveness detection on its own. */
    atomic_fetch_add(&progress_counter, 1);
}
