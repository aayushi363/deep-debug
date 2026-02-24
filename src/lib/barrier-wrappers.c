#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>

#include "mcmini/mcmini.h"
#include "deadlock_detector.h"

int mc_pthread_barrier_init(pthread_barrier_t *barrier,
                             const pthread_barrierattr_t *attr,
                             unsigned count) {
  MEASURE_FUNCTION_TIME
  log_debug("mc_pthread_barrier_init");

  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case CHECKPOINT_THREAD: {
      return libpthread_barrier_init(barrier, attr, count);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      rec_list *barrier_record = get_or_create_object_record(
          barrier, BARRIER, BARRIER_UNINITIALIZED);

      PAUSE_TIMER
      int rc = libpthread_barrier_init(barrier, attr, count);
      RESUME_TIMER
      if (rc == 0) {
        libpthread_mutex_lock(&barrier_record->node_lock);
        barrier_record->vo.bar_state.status     = BARRIER_INITIALIZED;
        barrier_record->vo.bar_state.count      = count;
        barrier_record->vo.bar_state.arrived    = 0;
        barrier_record->vo.bar_state.generation = 0;
        libpthread_mutex_unlock(&barrier_record->node_lock);
        deadlock_detector_increment_progress();
      }
      return rc;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART:
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = BARRIER_INIT_TYPE;
      memcpy_v(mb->cnts, &barrier, sizeof(barrier));
      memcpy_v(mb->cnts + sizeof(barrier), &count, sizeof(count));
      is_in_restart_mode() ? thread_handle_after_dmtcp_restart()
                           : thread_wake_scheduler_and_wait();
      return libpthread_barrier_init(barrier, attr, count);
    }
    default: {
      fprintf(stderr, "mcmini internal error: %s:%d\n", __FILE__, __LINE__);
      libc_abort();
    }
  }
}

int mc_pthread_barrier_destroy(pthread_barrier_t *barrier) {
  MEASURE_FUNCTION_TIME
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case CHECKPOINT_THREAD: {
      return libpthread_barrier_destroy(barrier);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      rec_list *barrier_record = get_or_create_object_record(
          barrier, BARRIER, BARRIER_UNINITIALIZED);

      PAUSE_TIMER
      int rc = libpthread_barrier_destroy(barrier);
      RESUME_TIMER
      if (rc == 0) {
        libpthread_mutex_lock(&barrier_record->node_lock);
        barrier_record->vo.bar_state.status = BARRIER_DESTROYED;
        libpthread_mutex_unlock(&barrier_record->node_lock);
        deadlock_detector_increment_progress();
      }
      return rc;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART:
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = BARRIER_DESTROY_TYPE;
      memcpy_v(mb->cnts, &barrier, sizeof(barrier));
      is_in_restart_mode() ? thread_handle_after_dmtcp_restart()
                           : thread_wake_scheduler_and_wait();
      return libpthread_barrier_destroy(barrier);
    }
    default: {
      fprintf(stderr, "mcmini internal error: %s:%d\n", __FILE__, __LINE__);
      libc_abort();
    }
  }
}

int mc_pthread_barrier_wait(pthread_barrier_t *barrier) {
  MEASURE_FUNCTION_TIME
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case CHECKPOINT_THREAD: {
      return libpthread_barrier_wait(barrier);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      rec_list *barrier_record = get_or_create_object_record(
          barrier, BARRIER, BARRIER_UNINITIALIZED);

      if (barrier_record->vo.bar_state.status != BARRIER_INITIALIZED) {
        fprintf(stderr, "Undefined behavior: attempting to wait on an "
                "uninitialized or destroyed barrier %p\n", (void *)barrier);
        libc_abort();
      }

      // Non-blocking approach using pthread_cond_timedwait, matching the
      // patterns used by mutex_lock (mutex_timedlock) and sem_wait
      // (sem_timedwait).  Since pthread_barrier_timedwait() does not exist in
      // POSIX, we simulate a timed barrier via a condition variable stored in
      // the rec_list node together with a generation counter in bar_state.
      //
      // The generation counter advances every time the barrier fires.  Each
      // thread records its generation ticket before blocking, then waits for
      // the counter to advance.  The last arriving thread broadcasts to wake
      // all sleepers.  A 1-second timeout allows threads to detect DMTCP
      // restart mode and fall through to the model-checking (TARGET_BRANCH)
      // path without blocking forever.

      libpthread_mutex_lock(&barrier_record->node_lock);
      unsigned my_generation = barrier_record->vo.bar_state.generation;
      barrier_record->vo.bar_state.arrived++;
      unsigned arrived = barrier_record->vo.bar_state.arrived;
      unsigned count   = barrier_record->vo.bar_state.count;

      if (arrived == count) {
        // This thread completes the barrier: reset counter, advance
        // generation, and wake all threads sleeping on the condition.
        barrier_record->vo.bar_state.arrived = 0;
        barrier_record->vo.bar_state.generation++;
        libpthread_cond_broadcast(&barrier_record->node_cond);
        libpthread_mutex_unlock(&barrier_record->node_lock);
        deadlock_detector_increment_progress();
        return PTHREAD_BARRIER_SERIAL_THREAD;
      }

      // Not the last thread: sleep until the generation advances (barrier
      // fires) or until a timeout fires and DMTCP restart is detected.
      while (barrier_record->vo.bar_state.generation == my_generation) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        PAUSE_TIMER
        int rc = libpthread_cond_timedwait(&barrier_record->node_cond,
                                           &barrier_record->node_lock, &ts);
        RESUME_TIMER
        if (rc == ETIMEDOUT && is_in_restart_mode()) {
          // DMTCP has restarted: escape the wait and fall through to the
          // model-checking path below.
          break;
        }
        // rc == 0 (broadcast woke us) or ETIMEDOUT without restart: re-check
        // the generation condition at the top of the loop.
      }
      libpthread_mutex_unlock(&barrier_record->node_lock);

      if (!is_in_restart_mode()) {
        // Normal completion: the generation advanced because the barrier fired.
        deadlock_detector_increment_progress();
        return 0;
      }

      // Explicit fallthrough into model-checking mode after DMTCP restart.
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART:
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();

      // Phase 1: barrier_arrive (always enabled).
      // Registers this thread's arrival with the model checker.  This
      // transition is always enabled and commutative with other arrivals,
      // so the scheduler can admit it immediately.
      mb->type = BARRIER_ARRIVE_TYPE;
      memcpy_v(mb->cnts, &barrier, sizeof(barrier));
      is_in_restart_mode() ? thread_handle_after_dmtcp_restart()
                           : thread_wake_scheduler_and_wait();

      // Phase 2: barrier_pass (gating).
      // Only enabled once all N threads have sent BARRIER_ARRIVE_TYPE for
      // this barrier (i.e. the model barrier is satisfied).  The scheduler
      // will not dispatch this thread past this point until every required
      // arrival has been recorded.
      mb = thread_get_mailbox();
      mb->type = BARRIER_PASS_TYPE;
      memcpy_v(mb->cnts, &barrier, sizeof(barrier));
      thread_wake_scheduler_and_wait();

      return 0;
    }
    default: {
      fprintf(stderr, "mcmini internal error: %s:%d\n", __FILE__, __LINE__);
      libc_abort();
    }
  }
}
