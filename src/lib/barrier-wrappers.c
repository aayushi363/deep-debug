#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>

#include "mcmini/mcmini.h"
#include "deadlock_detector.h"

/*
 * MCMINI_MODEL_BARRIERS: when set to a non-empty, non-zero value, McMini
 * intercepts and model-checks pthread_barrier_* calls. When unset (the
 * default), barriers pass straight through to libpthread — useful when
 * running programs that use barriers internally (e.g. DMTCP or OpenMP
 * runtimes) without wanting McMini to model them.
 */
static int barriers_enabled = 0;
static pthread_once_t barriers_once = PTHREAD_ONCE_INIT;

static void barriers_init_once(void) {
  const char *e = getenv("MCMINI_MODEL_BARRIERS");
  barriers_enabled = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
}

static int barriers_is_enabled(void) {
  pthread_once(&barriers_once, barriers_init_once);
  return barriers_enabled;
}

int mc_pthread_barrier_init(pthread_barrier_t *barrier,
                             const pthread_barrierattr_t *attr,
                             unsigned count) {
  MEASURE_FUNCTION_TIME

  if (!barriers_is_enabled())
    return libpthread_barrier_init(barrier, attr, count);

  log_debug("mc_pthread_barrier_init");

  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD: {
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

  if (!barriers_is_enabled())
    return libpthread_barrier_destroy(barrier);

  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD: {
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

  if (!barriers_is_enabled())
    return libpthread_barrier_wait(barrier);

  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD: {
      return libpthread_barrier_wait(barrier);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      rec_list *barrier_record = get_or_create_object_record(
          barrier, BARRIER, BARRIER_UNINITIALIZED);

      libpthread_mutex_lock(&barrier_record->node_lock);
      unsigned my_generation = barrier_record->vo.bar_state.generation;
      barrier_record->vo.bar_state.arrived++;

      if (barrier_record->vo.bar_state.arrived >=
          barrier_record->vo.bar_state.count) {
        // Last thread: advance generation and wake all waiters.
        barrier_record->vo.bar_state.arrived = 0;
        barrier_record->vo.bar_state.generation++;
        libpthread_cond_broadcast(&barrier_record->node_cond);
        libpthread_mutex_unlock(&barrier_record->node_lock);
        deadlock_detector_increment_progress();
        return PTHREAD_BARRIER_SERIAL_THREAD;
      }

      // Not the last thread: wait until generation advances (barrier fires)
      // or until a timeout fires and DMTCP restart is detected.
      while (barrier_record->vo.bar_state.generation == my_generation) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        PAUSE_TIMER
        int rc = libpthread_cond_timedwait(&barrier_record->node_cond,
                                           &barrier_record->node_lock, &ts);
        RESUME_TIMER
        if (rc == ETIMEDOUT && is_in_restart_mode()) {
          break;
        }
      }
      libpthread_mutex_unlock(&barrier_record->node_lock);

      if (!is_in_restart_mode()) {
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

      mb->type = BARRIER_ARRIVE_TYPE;
      memcpy_v(mb->cnts, &barrier, sizeof(barrier));
      is_in_restart_mode() ? thread_handle_after_dmtcp_restart()
                           : thread_wake_scheduler_and_wait();

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
