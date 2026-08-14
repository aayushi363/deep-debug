#define _GNU_SOURCE
#include <assert.h>
#include <dmtcp.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mcmini/common/exit.h"
#include "mcmini/Thread_queue.h"
#include "mcmini/mcmini.h"
#include "mcmini/wrapper_timing.h"
#include "mcmini/spy/checkpointing/lockset.h"
#include "deadlock_detector.h"
#include "json_events.h"

// This function will run automatically when libmcmini.so is loaded.
__attribute__((constructor))
static void initialize_timer_on_load(void) {
  const char* home_dir = getenv("HOME");
  char* report_path = NULL;

  if (home_dir) {
    // Allocate a larger buffer to be safe with numbered suffixes.
    // This can handle path lengths up to strlen(home_dir) + 63 characters.
    report_path = malloc(strlen(home_dir) + 64);
    if (!report_path) {
        // Handle malloc failure
        timer_init("/tmp/mcmini-timing-report.txt");
        return;
    }

    // Start with the base filename.
    sprintf(report_path, "%s/mcmini-timing-report.txt", home_dir);

    // Check if the file already exists using access(). F_OK checks for existence.
    // If access() returns 0, the file exists.
    if (access(report_path, F_OK) == 0) {
      int i = 1;
      // Loop to find a unique filename, e.g., mcmini-timing-report(1).txt
      // We'll try up to 999 times to avoid an infinite loop.
      for (i = 1; i < 1000; ++i) {
        sprintf(report_path, "%s/mcmini-timing-report-%d.txt", home_dir, i);
        // If we find a filename that does NOT exist, break the loop.
        if (access(report_path, F_OK) != 0) {
          break;
        }
      }
    }
  } else {
    // Fallback if HOME isn't set. For simplicity, we don't add numbering here,
    // but the same logic could be applied.
    const char* fallback_path = "/tmp/mcmini-timing-report.txt";
    report_path = malloc(strlen(fallback_path) + 1);
    if (!report_path) {
        timer_init(NULL); // Let timer_init handle the NULL case
        return;
    }
    strcpy(report_path, fallback_path);
  }

  timer_init(report_path);

}

// This function will run automatically when the program exits or the library is unloaded.
__attribute__((destructor))
static void save_report_on_unload(void) {
  // The global g_report_filepath is already set by the constructor.
  // The 'save_timing_report' function in timer.h uses this global variable.
  save_timing_report(NULL); // Pass NULL to use the globally configured path.
}

typedef struct pthread_map {
    pthread_t thread;
    runner_id_t value;
    struct pthread_map *next;
} pthread_map_t;

static pthread_rwlock_t pthread_map_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_map_t *head = NULL;

void insert_pthread_map(pthread_t t, runner_id_t v) {
    pthread_rwlock_wrlock(&pthread_map_lock);
    pthread_map_t *n = malloc(sizeof *n);
    n->thread = t;
    n->value = v;
    n->next = head;
    head = n;
    pthread_rwlock_unlock(&pthread_map_lock);
}

runner_id_t search_pthread_map(pthread_t t) {
  pthread_rwlock_rdlock(&pthread_map_lock);
    pthread_map_t *cur = head;
    while (cur) {
        if (pthread_equal(cur->thread, t)) {
            // BUGFIX: release the read-lock on the hit path too. The original
            // returned here still holding the rdlock, so the NEXT
            // insert_pthread_map (write-lock) — e.g. the second pthread_create's
            // child registering — would block forever, wedging the scheduler.
            runner_id_t v = cur->value;
            pthread_rwlock_unlock(&pthread_map_lock);
            return v;
        }
        cur = cur->next;
    }
    pthread_rwlock_unlock(&pthread_map_lock);
    return RID_INVALID;
}


MCMINI_THREAD_LOCAL runner_id_t tid_self = RID_INVALID;

runner_id_t mc_register_this_thread(void) {
  MEASURE_FUNCTION_TIME
  static pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
  static runner_id_t tid_next = 0;

  // Lazily enable deadlock detector when a thread registers while the
  // library is in RECORD or PRE_CHECKPOINT mode. This ensures the detector
  // is active only during Phase I recording.
  switch (get_current_mode()) {
    case RECORD:
    case PRE_CHECKPOINT: {
      mc_install_deadlock_detector(true);
      break;
    }
    default:
      break;
  }

  libpthread_mutex_lock(&mut);
  tid_self = tid_next++;
  insert_pthread_map(pthread_self(), tid_self);
  libpthread_mutex_unlock(&mut);
  return tid_self;
}

volatile runner_mailbox *thread_get_mailbox() {
  assert(!is_checkpoint_thread());
  assert(tid_self != RID_INVALID);
  assert(tid_self != RID_CHECKPOINT_THREAD);
  return &((volatile struct mcmini_shm_file *)(global_shm_start))
              ->mailboxes[tid_self];
}

void thread_wake_scheduler_and_wait(void) {
  MEASURE_FUNCTION_TIME
  log_verbose("thread_wake_scheduler_and_wait\n");
  fflush(stdout);
  assert(tid_self != RID_INVALID);
  volatile runner_mailbox *thread_mailbox = thread_get_mailbox();
  errno = 0;
  int wake_rc = mc_wake_scheduler(thread_mailbox);
  assert(wake_rc == 0);
  assert(errno == 0);

  errno = 0;
  int rc = mc_wait_for_scheduler(thread_mailbox);
  while (rc != 0 && errno == EINTR) {
    rc = mc_wait_for_scheduler(thread_mailbox);
  }
  assert(errno != EINVAL);
}

void thread_await_scheduler(void) {
  assert(tid_self != RID_INVALID);
  volatile runner_mailbox *thread_mailbox = thread_get_mailbox();

  errno = 0;
  int rc = mc_wait_for_scheduler(thread_mailbox);
  while (rc != 0 && errno == EINTR) {
    rc = mc_wait_for_scheduler(thread_mailbox);
  }
  assert(errno != EINVAL);
}

void thread_awake_scheduler_for_thread_finish_transition(void) {
  assert(tid_self != RID_INVALID);
  mc_wake_scheduler(thread_get_mailbox());
}

void thread_block_indefinitely(void) {
  while (1) {
    pause();
  }
}

int mc_pthread_mutex_init(pthread_mutex_t *mutex,
                          const pthread_mutexattr_t *attr) {
  // FIXME: Only handles NORMAL mutexes
  if (mc_in_wrapper) return libpthread_mutex_init(mutex, attr);

  MEASURE_FUNCTION_TIME
  if (attr != NULL) {
    int type;
    pthread_mutexattr_gettype(attr, &type);
    assert(type == PTHREAD_MUTEX_NORMAL);
  }

  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case CHECKPOINT_THREAD:
    case FUZZER_STANDALONE: {
      // M2.1: single-step through the in-SUT scheduler. Announce the op, park
      // until the scheduler releases us, then log (in scheduled order) + do it.
      mc_in_wrapper = 1;
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_INIT_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      write_pthread_event("MUTEX_INIT",
                      get_json_thread_id(),
                      (uint32_t)(uintptr_t)mutex,
                      next_json_seqno());
      int rc = libpthread_mutex_init(mutex, attr);
      mc_in_wrapper = 0;
      return rc;
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      // libpthread_mutex_lock(&rec_list_lock);
      // rec_list *mutex_record = find_object_record_mode(mutex);
      // if (mutex_record == NULL) {
      //   // FIXME: We assume that this is a normal mutex. For other mutex
      //   // types, we'd need to behave differently
      //   visible_object vo = {
      //       .type = MUTEX, .location = mutex, .mut_state = UNINITIALIZED};
      //   mutex_record = add_rec_entry_record_mode(&vo);
      // }
      // libpthread_mutex_unlock(&rec_list_lock);
      rec_list *mutex_record = get_or_create_object_record(mutex, MUTEX, UNINITIALIZED);
      PAUSE_TIMER
      int rc = libpthread_mutex_init(mutex, attr);
      RESUME_TIMER
      if (rc == 0) {  // Init
        // libpthread_mutex_lock(&rec_list_lock);
        libpthread_mutex_lock(&mutex_record->node_lock);
        mutex_record->vo.mut_state = UNLOCKED;
        //libpthread_mutex_unlock(&rec_list_lock);
        libpthread_mutex_unlock(&mutex_record->node_lock);
        /* Notify deadlock detector that visible progress occurred. */
        deadlock_detector_increment_progress();
      }
      return rc;
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_INIT_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_handle_after_dmtcp_restart();
      return libpthread_mutex_init(mutex, attr);
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_INIT_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      return libpthread_mutex_init(mutex, attr);
    }
    default: {
      // Wrapper functions should not be executing
      // inside the template! If we reach this point, it
      // means that this is a template process. This
      // method must have been directly called
      // erroneously.
      libc_abort();
    }
  }
}

int mc_pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (mc_in_wrapper) {
    return libpthread_mutex_lock(mutex);   // nested — pass through, no emit
      }
  MEASURE_FUNCTION_TIME
  // On entry, there are several cases:
  //
  // 1. The thread was executing before dmtcp has had a chance
  // to notify us of the DMTCP_EVENT_INIT. Here we forward
  // the call to `libpthread.so` directly to avoid recording
  // the system calls dmtcp makes in order to set up
  // restart (i.e. prior to invoking our callback)
  //
  // 2. The thread was executing invisible operations during
  // restart and has since reached the next visible operation.
  // In this case, before shifting into model checking mode, we
  // need to notify the template thread that the thread has
  // completed all its recording.
  //
  // 3. The thread was executing a visible operation and has since
  // reached its next visible operation as a consequence of
  // control by the scheduler. Direct transfer to model checking
  // mode suffices.
  //
  // 4. The thread is executing in RECORD mode and should do state
  // tracking.
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case CHECKPOINT_THREAD:
    case FUZZER_STANDALONE: {

      // M2.1: announce LOCK, park; the scheduler only releases us when the
      // mutex is free, so the real lock below never blocks.
      mc_in_wrapper = 1;
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_LOCK_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      write_pthread_event("MUTEX_LOCK",
                          get_json_thread_id(),
                          (uint32_t)(uintptr_t)mutex,
                          next_json_seqno());
      int rc = libpthread_mutex_lock(mutex);
      mc_in_wrapper = 0;
      return rc;
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      // NOTE: This is subtle: at this point, the possible modes are
      // RECORD ***AND*** DMTCP_RESTART. The latter is possible if
      // checkpointing occurs anywhere AFTER the switch statement above.
      // libpthread_mutex_lock(&rec_list_lock);
      // rec_list *mutex_record = find_object_record_mode(mutex);
      // if (mutex_record == NULL) {
      //   visible_object vo = {
      //       .type = MUTEX, .location = mutex, .mut_state = UNINITIALIZED};
      //   mutex_record = add_rec_entry_record_mode(&vo);
      // }
      // libpthread_mutex_unlock(&rec_list_lock);
      rec_list *mutex_record = get_or_create_object_record(mutex, MUTEX, UNINITIALIZED);
      struct timespec time = {.tv_sec = 2};
      while (1) {
        PAUSE_TIMER
        int rc = libpthread_mutex_timedlock(mutex, &time);
        RESUME_TIMER
        if (rc == 0) {  // Lock succeeded
          // libpthread_mutex_lock(&rec_list_lock);
          libpthread_mutex_lock(&mutex_record->node_lock);
          mutex_record->vo.mut_state = LOCKED;
          // libpthread_mutex_unlock(&rec_list_lock);
          libpthread_mutex_unlock(&mutex_record->node_lock);
          /* Phase-1 lockset predictor: this thread now holds `mutex`. */
          lockset_acquire(mutex);
          /* Notify deadlock detector that visible progress occurred. */
          deadlock_detector_increment_progress();
          return rc;
        } else if (rc == ETIMEDOUT) {  // If the lock failed.
          // Here, the user-space thread did not manage to acquire
          // the lock. However, we do NOT want threads to block during
          // the recording phase to ensure that each user-space thread
          // can be put back under the control of the model checker.
          //
          // For those threads which have not managed to acquire the lock,
          // we want to ensure that they can eventually escape from this loop.
          // After the DMTCP_EVENT_RESTART event, exactly one thread will
          // successfully acquire the lock. Other threads must notice that
          // the record phase has ended or else they could loop forever.
          if (is_in_restart_mode()) {
            break;
          }
        } else if (rc != 0 && rc != ETIMEDOUT) {
          // A "true" error: something went wrong with locking
          // and we pass this on to the end user
          return rc;
        }
      }
      // Explicit fallthrough
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_LOCK_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_handle_after_dmtcp_restart();
      return libpthread_mutex_lock(mutex);
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_LOCK_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      return libpthread_mutex_lock(mutex);
    }
    default: {
      // Wrapper functions should not be executing
      // inside the template! If we reach this point, it
      // means that this is a template process. This
      // method must have been directly called
      // erroneously.
      libc_abort();
    }
  }
}

int mc_pthread_mutex_unlock(pthread_mutex_t *mutex) {
  if (mc_in_wrapper) return libpthread_mutex_unlock(mutex);
  MEASURE_FUNCTION_TIME
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case CHECKPOINT_THREAD:
    case FUZZER_STANDALONE: {
      
      // M2.1: announce UNLOCK (always enabled), park, then unlock.
      mc_in_wrapper = 1;
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_UNLOCK_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
       write_pthread_event("MUTEX_UNLOCK",
                      get_json_thread_id(),
                      (uint32_t)(uintptr_t)mutex,
                      next_json_seqno());
      int rc = libpthread_mutex_unlock(mutex);
      mc_in_wrapper = 0;
      return rc;
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      // libpthread_mutex_lock(&rec_list_lock);
      // rec_list *mutex_record = find_object_record_mode(mutex);
      // if (mutex_record == NULL) {
      //   // FIXME: We assume that this is a normal mutex. For other mutex
      //   // types, we'd need to behave differently
      //   fprintf(stderr,
      //           "Undefined behavior: attempting to unlock an uninitialized "
      //           "mutex %p",
      //           mutex);
      //   libc_abort();
      // }
      rec_list *mutex_record = get_or_create_object_record(mutex, MUTEX, UNINITIALIZED);
      PAUSE_TIMER
      // libpthread_mutex_unlock(&rec_list_lock);
      int rc = libpthread_mutex_unlock(mutex);
      RESUME_TIMER
      if (rc == 0) {  // Unlock succeeded
        // libpthread_mutex_lock(&rec_list_lock);
        libpthread_mutex_lock(&mutex_record->node_lock);
        mutex_record->vo.mut_state = UNLOCKED;
        // libpthread_mutex_unlock(&rec_list_lock);
        libpthread_mutex_unlock(&mutex_record->node_lock);
        /* Phase-1 lockset predictor: this thread no longer holds `mutex`. */
        lockset_release(mutex);
        /* Notify deadlock detector that visible progress occurred. */
        deadlock_detector_increment_progress();
      }
      return rc;
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_UNLOCK_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_handle_after_dmtcp_restart();
      return libpthread_mutex_unlock(mutex);
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_UNLOCK_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      return libpthread_mutex_unlock(mutex);
    }
    default: {
      // Wrapper functions should not be executing
      // inside the template! If we reach this point, it
      // means that this is a template process. This
      // method must have been directly called
      // erroneously.
      libc_abort();
    }
  }
}

void mc_exit_thread_in_child(void) {
  thread_get_mailbox()->type = THREAD_EXIT_TYPE;
  thread_wake_scheduler_and_wait();
  thread_awake_scheduler_for_thread_finish_transition();
  thread_block_indefinitely();
}

void mc_exit_main_thread_in_child(void) {
  if (tid_self != RID_MAIN_THREAD) libc_abort();
  // IMPORTANT: This is NOT a typo!
  // 1. `thread_wake_scheduler_and_wait()` is called when the
  // main thread is known to be _alive_ to the model
  // 2. `thread_awake_scheduler_for_thread_finish_transition()`
  // is called to simulate the thread having "exited"
  // 3. `thread_block_indefinitely()` ensures that the
  // main thread never actually exits
  //
  // However, unlike a normal thread exiting, we want to ensure that
  // the process doesn't terminate; hence, we prevent the main thread
  // from ever escaping this function.
  thread_get_mailbox()->type = THREAD_EXIT_TYPE;
  thread_wake_scheduler_and_wait();

  thread_get_mailbox()->type = THREAD_EXIT_TYPE;
  thread_awake_scheduler_for_thread_finish_transition();
  thread_block_indefinitely();
}

MCMINI_NO_RETURN void mc_transparent_exit(int status) {
  // No checkpoints within the true scope. Since we want to perform
  // the true `exit(2)` call even in the case of recording,
  // we need to ensure that a restarted process can't initiate
  // a checkpoint in the call to `libc_exit()`; otherwise the
  // checkpoint image would be "permanently" bad and always exit
  // before we have a chance to restore it and explore the (short)
  // branch leading to the exit. The same logic applies for `abort(2)`

  switch (get_current_mode()) {
    case RECORD:
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case PRE_CHECKPOINT: {
      /* Notify deadlock detector that visible progress occurred (process exiting). */
      deadlock_detector_increment_progress();
      libc_exit(status);
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = PROCESS_EXIT_TYPE;
      memcpy_v(mb->cnts, &status, sizeof(status));
      thread_handle_after_dmtcp_restart();

      // Fallthrough
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = PROCESS_EXIT_TYPE;
      memcpy_v(mb->cnts, &status, sizeof(status));
      thread_await_scheduler();

      // After "exiting", don't actually exit yet:
      // the model checker will prevent the process
      // from continuing execution and will note this
      // branch as "useless" since at this point
      mb->type = PROCESS_EXIT_TYPE;
      memcpy_v(mb->cnts, &status, sizeof(status));
      thread_await_scheduler();
    }
    default: {
      libc_exit(status);
    }
  }
}

MCMINI_NO_RETURN void mc_transparent_abort(void) {
  // No checkpoints within the true scope. Since we want to perform
  // the true `abort(2)` call even in the case of recording,
  // we need to ensure that a restarted process can't initiate
  // a checkpoint in the call to `libc_exit()`; otherwise the
  // checkpoint image would be "permanently" bad and always exit
  // before we have a chance to restore it and explore the (short)
  // branch leading to the exit. The same logic applies for `abort(2)`
  // dmtcp_disable_ckpt();

  switch (get_current_mode()) {
    case RECORD:
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case PRE_CHECKPOINT: {
      /* Notify deadlock detector that visible progress occurred (process abort). */
      deadlock_detector_increment_progress();
      libc_abort();
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = PROCESS_ABORT_TYPE;
      thread_handle_after_dmtcp_restart();

      // Explicit fallthrough
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = PROCESS_ABORT_TYPE;
      thread_wake_scheduler_and_wait();

      // After "aborting", don't actually abort yet:
      // the model checker will prevent the process
      // from continuing execution and will note this
      // branch as "useless" since at this point
      mb->type = PROCESS_ABORT_TYPE;
      thread_wake_scheduler_and_wait();
      libc_abort();
    }
    default: {
      libc_abort();
    }
  }
}

struct mc_thread_routine_arg {
  void *arg;
  thread_routine routine;
  sem_t mc_pthread_create_binary_sem;
};

void *mc_thread_routine_wrapper(void *arg) {
  runner_id_t rid = mc_register_this_thread();
  struct mc_thread_routine_arg *unwrapped_arg = arg;
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case CHECKPOINT_THREAD: {
      fprintf(stderr,
      "In `PRE_DMTCP_INIT` mode, `mc_pthread_create` always directly calls DMTCP."
      "Reaching this point would be an error.\n");
      libc_abort();
    }
    case RECORD:
    case PRE_CHECKPOINT:
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      // If we've noticed we're executing after a `DMTCP_EVENT_RESTART`, we
      // simply let the thread continue executing until one of two things
      // happens:
      //
      // 1. The thread routine itself calls another wrapper function (i.e.
      // visible operation)
      // 2. The thread routine exits before calling another wrapper function.
      //
      // The same applies to RECORD mode: we simply do the recording and let the
      // thread routine call the next wrapper function.
      pthread_t this_thread = pthread_self();
      // libpthread_mutex_lock(&rec_list_lock);
      // rec_list *thread_record = find_thread_record_mode(this_thread);
      // assert(thread_record == NULL);
      // visible_object vo = {.type = THREAD,
      //                      .location = NULL,
      //                      .thrd_state.pthread_desc = this_thread,
      //                      .thrd_state.status = ALIVE,
      //                      .thrd_state.id = rid};
      // thread_record = add_rec_entry_record_mode(&vo);
      // libpthread_mutex_unlock(&rec_list_lock);
      get_or_create_thread_record(this_thread, rid, ALIVE);
      /* Notify deadlock detector that visible progress occurred (thread created). */
      deadlock_detector_increment_progress();
      libpthread_sem_post(&unwrapped_arg->mc_pthread_create_binary_sem);
      break;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      libpthread_sem_post(&unwrapped_arg->mc_pthread_create_binary_sem);
      // Simulates THREAD_START for this thread NOTE:
      // We don't need to write into shared memory here. The
      // scheduler already knows how to handle the case
      // of thread creation
      thread_await_scheduler();
      break;
    }
    case FUZZER_STANDALONE: {
      // M2.1: this new runner is already registered (top of this function).
      // Signal the creator it exists, then park until the scheduler releases us
      // for the first time; only then run the routine body. This keeps the
      // scheduler in control from the thread's very first step.
      libpthread_sem_post(&unwrapped_arg->mc_pthread_create_binary_sem);
      thread_await_scheduler();
      mc_in_wrapper = 1;
      write_pthread_event("THREAD_START",
                      get_json_thread_id(),
                      (uint32_t)(uintptr_t)pthread_self(),
                      next_json_seqno());
      mc_in_wrapper = 0;
      break;
    }
    default: {
      libc_abort();
    }
  }
  void *rv = unwrapped_arg->routine(unwrapped_arg->arg);
  free(arg);

  switch (get_current_mode()) {
    case RECORD:
    case PRE_CHECKPOINT: {
      // libpthread_mutex_lock(&rec_list_lock);
      pthread_rwlock_rdlock(&rec_list_lock);
      rec_list *thread_record = find_thread_record_mode(pthread_self());
      pthread_rwlock_unlock(&rec_list_lock);
      assert(thread_record != NULL);
      libpthread_mutex_lock(&thread_record->node_lock);
      thread_record->vo.thrd_state.status = EXITED;
      fprintf(stdout, "\n\nexited\n\n"); fflush(stdout);
      libpthread_mutex_unlock(&thread_record->node_lock);
      // libpthread_mutex_unlock(&rec_list_lock);
  /* Notify deadlock detector that visible progress occurred (thread exited). */
  deadlock_detector_increment_progress();
  return rv;
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      thread_get_mailbox()->type = THREAD_EXIT_TYPE;
      thread_handle_after_dmtcp_restart();

      thread_get_mailbox()->type = THREAD_EXIT_TYPE;
      thread_await_scheduler();
      break;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      mc_exit_thread_in_child();
      break;
    }
    case FUZZER_STANDALONE: {
      // M2.1: log the exit, then announce THREAD_EXIT through the mailbox and
      // block. mc_exit_thread_in_child() wakes the scheduler, waits for release,
      // wakes it once more (finish transition), and blocks forever — the
      // scheduler marks this runner EXITED. It does not return.
      mc_in_wrapper = 1;
      write_pthread_event("THREAD_EXIT",
                      get_json_thread_id(),
                      (uint32_t)(uintptr_t)pthread_self(),
                      next_json_seqno());
      mc_in_wrapper = 0;
      mc_exit_thread_in_child();
      break;
    }
    default: {
      libc_abort();
    }
  }
  return rv;
}

void record_main_thread(void) {
  pthread_t main_thread = pthread_self();
  runner_id_t main_tid = mc_register_this_thread();
  assert(main_tid == 0);
  // libpthread_mutex_lock(&rec_list_lock);
  // rec_list *thread_record = find_thread_record_mode(main_thread);
  // assert(thread_record == NULL);
  // visible_object vo = {.type = THREAD,
  //                      .location = NULL,
  //                      .thrd_state.id = main_tid,
  //                      .thrd_state.pthread_desc = main_thread,
  //                      .thrd_state.status = ALIVE};
  // thread_record = add_rec_entry_record_mode(&vo);
  get_or_create_thread_record(main_thread, main_tid, ALIVE);
  // libpthread_mutex_unlock(&rec_list_lock);
}

void record_checkpoint_thread(void) {
  tid_self = RID_CHECKPOINT_THREAD;
  ckpt_pthread_descriptor = pthread_self();
  atomic_store(&libmcmini_has_recorded_checkpoint_thread, true);

  // Recording the presence of the checkpoint thread means that the main
  // thread has made its first call to `pthread_create`. The assumption
  // is that DMTCP (executing in the main thread before the `main` routine)
  // makes the first call to `pthread_create` to create the checkpoint thread.
  // Since the checkpoint thread is about to be created, it is safe to begin
  // recording.
  set_current_mode(RECORD);
}

void *dmtcp_create_checkpoint_thread_wrapper(void *arg) {
  record_checkpoint_thread();
  struct mc_thread_routine_arg *unwrapped_arg = arg;
  libpthread_sem_post(&unwrapped_arg->mc_pthread_create_binary_sem);
  void *rv = unwrapped_arg->routine(unwrapped_arg->arg);
  free(arg);
  return rv;
}

int mc_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*routine)(void *), void *arg) {
  // NOTE: We're assuming that DMTCP creates only the checkpoint thread
  // immediately after sending the `DMTCP_EVENT_INIT` to `libmcmini.so`
  // and creates no other threads during execution
  if (mc_in_wrapper) {
        return libpthread_pthread_create(thread, attr, routine, arg);
      }
  static pthread_once_t main_thread_once = PTHREAD_ONCE_INIT;

  // TODO: Reduce code duplication here!
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT: {
      // This case implies that DMTCP attempted to create
      // a thread BEFORE the `DMTCP_EVENT_INIT` was delivered
      // to `libmcmini`'s callback. This
      // NOTE: Explicit fallthrough intended
      assert(0);
    }
    case PRE_CHECKPOINT_THREAD: {
      pthread_once(&main_thread_once, &record_main_thread);

      // We must be able to at runtime determine which thread is the checkpoint
      // thread. Here we record the `pthread_t` struct assigned to the
      // checkpoint thread and later compare it with the value returned by
      // `pthread_create()`.
      //
      // NOTE: The semaphore is necessary here as the child thread in this
      // instance will be the checkpoint thread and will hence call into DMTCP.
      // Since the goal of detecting if the caller is the checkpoint thread
      // inside of wrappers is to prevent the checkpoint thread from interacting
      // with McMini wrappers, and since we write the checkpoint thread's
      // `pthread_t` into a globally accessible location, we must synchronize
      // with the checkpoint thread.
      struct mc_thread_routine_arg *wrapped_arg =
          malloc(sizeof(struct mc_thread_routine_arg));
      wrapped_arg->arg = arg;
      wrapped_arg->routine = routine;
      libpthread_sem_init(&wrapped_arg->mc_pthread_create_binary_sem, 0, 0);
      int rc = libdmtcp_pthread_create(
          thread, attr, &dmtcp_create_checkpoint_thread_wrapper, wrapped_arg);
      libpthread_sem_wait(&wrapped_arg->mc_pthread_create_binary_sem);
      return rc;
    }
    case CHECKPOINT_THREAD: {
      log_warn(
          "The checkpoint thread is creating another thread. Calls from this "
          "thread should probably be ignored by the McMini library, as "
          "creating a thread indicates the checkpoint thread is doing "
          "background work. The DMTCP library and McMini must not interefere. "
          "However, the current implementation "
          "only prevents calls by the checkpoint thread from entering wrapper "
          "functions.");
      return libdmtcp_pthread_create(thread, attr, routine, arg);
    }
    case RECORD:
    case PRE_CHECKPOINT:
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      // TODO: add support for thread attributes
      struct mc_thread_routine_arg *libmcmini_controlled_thread_arg =
          malloc(sizeof(struct mc_thread_routine_arg));
      libmcmini_controlled_thread_arg->arg = arg;
      libmcmini_controlled_thread_arg->routine = routine;
      // TODO: Handle the errors that can occur when
      // pthread_create is called. They are unlikely to
      // occur in practice, but should be handled
      libpthread_sem_init(
          &libmcmini_controlled_thread_arg->mc_pthread_create_binary_sem, 0,
          0);
      const int rc =
          libdmtcp_pthread_create(thread, attr, &mc_thread_routine_wrapper,
                                    libmcmini_controlled_thread_arg);
      // IMPORTANT: We need to ensure that the child thread is recorded
      // before exiting; otherwise there are potential race conditions
      // in the record code (e.g. with `pthread_join()`) which expect the
      // child thread to have been recorded when it may not have been yet.
      libpthread_sem_wait(
          &libmcmini_controlled_thread_arg->mc_pthread_create_binary_sem);
      return rc;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      // TODO: add support for thread attributes
      struct mc_thread_routine_arg *libmcmini_controlled_thread_arg =
          malloc(sizeof(struct mc_thread_routine_arg));
      libmcmini_controlled_thread_arg->arg = arg;
      libmcmini_controlled_thread_arg->routine = routine;
      libpthread_sem_init(
          &libmcmini_controlled_thread_arg->mc_pthread_create_binary_sem, 0,
          0);

      // NOTE: This code is reached when the model checker has control.
      // Since this process represents an(emphemeral) branch of state space,
      // unless we later want to checkpoint this branch, we don't need to
      // inform DMTCP of these new threads. Indeed, in classic model checking
      // mode, `libdmtcp.so` is not even loaded (so we'd have to check first anyway).
      // Calling `libpthread_pthread_create` simplifies all this.
      const int rv =
          libpthread_pthread_create(thread, attr, &mc_thread_routine_wrapper,
                                    libmcmini_controlled_thread_arg);

      // IMPORTANT: We need to ensure that the thread that is
      // created has been assigned an id; otherwise, there is a race condition
      // in which two thread creates in the child might
      // not be scheduled to run until *two* steps of the scheduler
      libpthread_sem_wait(
          &libmcmini_controlled_thread_arg->mc_pthread_create_binary_sem);

      memcpy_v(thread_get_mailbox()->cnts, thread, sizeof(pthread_t));
      thread_get_mailbox()->type = THREAD_CREATE_TYPE;
      thread_wake_scheduler_and_wait();
      return rv;
    }
    case FUZZER_STANDALONE: {
      // M2.1: create the child (it registers + parks awaiting the scheduler),
      // wait until it has registered, then announce THREAD_CREATE with the
      // child's runner id so the scheduler adds it to the runnable set.
      mc_in_wrapper = 1;
      struct mc_thread_routine_arg *libmcmini_controlled_thread_arg =
          malloc(sizeof(struct mc_thread_routine_arg));
      libmcmini_controlled_thread_arg->arg = arg;
      libmcmini_controlled_thread_arg->routine = routine;
      libpthread_sem_init(
          &libmcmini_controlled_thread_arg->mc_pthread_create_binary_sem, 0, 0);
      const int rv =
          libpthread_pthread_create(thread, attr, &mc_thread_routine_wrapper,
                                    libmcmini_controlled_thread_arg);
      libpthread_sem_wait(
          &libmcmini_controlled_thread_arg->mc_pthread_create_binary_sem);
      runner_id_t child_rid = search_pthread_map(*thread);
      volatile runner_mailbox *mb = thread_get_mailbox();
      memcpy_v(mb->cnts, &child_rid, sizeof(runner_id_t));
      mb->type = THREAD_CREATE_TYPE;
      thread_wake_scheduler_and_wait();
      write_pthread_event("THREAD_CREATE",
                          get_json_thread_id(),
                          (uint32_t)(uintptr_t)(*thread),
                          next_json_seqno());
      mc_in_wrapper = 0;
      return rv;
    }
    default: {
      libc_abort();
    }
  }
}

int mc_pthread_join(pthread_t t, void **rv) {
  if (mc_in_wrapper) return libpthread_pthread_join(t, rv);

  switch (get_current_mode()) {
    case PRE_DMTCP_INIT: {
      // This case implies that DMTCP attempted to join
      // a thread BEFORE the `DMTCP_EVENT_INIT` was delivered
      // to `libmcmini`'s callback
      // NOTE: Explicit fallthrough intended
      assert(0);
    }
    case PRE_CHECKPOINT_THREAD:
    case CHECKPOINT_THREAD: {
      return libdmtcp_pthread_join(t, rv);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      // NOTE: This is subtle: at this point, the possible modes are
      // RECORD ***AND*** DMTCP_RESTART. The latter is possible if
      // checkpointing occurs anywhere AFTER the switch statement above.
      // libpthread_mutex_lock(&rec_list_lock);
      pthread_rwlock_rdlock(&rec_list_lock);
      rec_list *thread_record = find_thread_record_mode(t);
      pthread_rwlock_unlock(&rec_list_lock);
      assert(thread_record != NULL);
      // libpthread_mutex_unlock(&rec_list_lock);

      struct timespec time = {.tv_sec = 2, .tv_nsec = 0};
      while (1) {
        int rc = pthread_timedjoin_np(t, rv, &time);
        if (rc == 0) {  // Join succeeded
          // libpthread_mutex_lock(&rec_list_lock);
          libpthread_mutex_lock(&thread_record->node_lock);
          thread_record->vo.thrd_state.status = EXITED;
          // libpthread_mutex_unlock(&rec_list_lock);
          libpthread_mutex_unlock(&thread_record->node_lock);
          /* Notify deadlock detector that visible progress occurred. */
          deadlock_detector_increment_progress();
          return rc;
        } else if (rc == ETIMEDOUT) {
          // If the join failed.
          // Here, the user-space thread did not manage to join on
          // the thread. However, we do NOT want threads to block during
          // the recording phase to ensure that each user-space thread
          // can be put back under the control of the model checker.
          //
          // For those threads which have not managed to join on the thread,
          // we want to ensure that they can eventually escape from this loop.
          // After the DMTCP_EVENT_RESTART event, exactly one thread will
          // successfully acquire the lock. Other threads must notice that
          // the record phase has ended or else they would loop forever.
          if (is_in_restart_mode()) {
            break;
          }
        } else if (rc != 0 && rc != ETIMEDOUT) {
          // A "true" error: something went wrong with locking
          // and we pass this on to the end user
          return rc;
        }
      }
    }
    // Explicit fallthrough here
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      runner_id_t rid = search_pthread_map(t);
      memcpy_v(thread_get_mailbox()->cnts, &rid, sizeof(runner_id_t));
      thread_get_mailbox()->type = THREAD_JOIN_TYPE;
      thread_handle_after_dmtcp_restart();
      return 0;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      runner_id_t rid = search_pthread_map(t);
      memcpy_v(thread_get_mailbox()->cnts, &rid, sizeof(runner_id_t));
      thread_get_mailbox()->type = THREAD_JOIN_TYPE;
      thread_wake_scheduler_and_wait();
      return 0;
    }
    case FUZZER_STANDALONE: {
      // M2.1: announce JOIN(target). The scheduler only releases us once the
      // target has EXITED, so the join is logically satisfied; we return 0
      // without a real libpthread join (the target thread is parked forever in
      // mc_exit_thread_in_child, so a real join would block).
      mc_in_wrapper = 1;
      runner_id_t rid = search_pthread_map(t);
      volatile runner_mailbox *mb = thread_get_mailbox();
      memcpy_v(mb->cnts, &rid, sizeof(runner_id_t));
      mb->type = THREAD_JOIN_TYPE;
      thread_wake_scheduler_and_wait();
      write_pthread_event("THREAD_JOIN",
                      get_json_thread_id(),
                      (uint32_t)(uintptr_t)t,
                      next_json_seqno());
      mc_in_wrapper = 0;
      return 0;
    }
    default: {
      libc_abort();
    }
  }

}

unsigned mc_sleep(unsigned duration) {
  switch (get_current_mode()) {
    case TARGET_BRANCH:
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE:
    case TARGET_BRANCH_AFTER_RESTART: {
      // Ignore actually putting this thread to sleep:
      // it doesn't affect correctness neither for model
      // checking nor for state regenetation.
      return 0;
    }
    default:
      return libc_sleep(duration);
  }
}

int mc_pthread_cond_init(pthread_cond_t *cond,
                         const pthread_condattr_t *attr) {
  // Infrastructure threads (scheduler, checkpoint, libvoidstar-internal) run
  // with mc_in_wrapper=1 and never register a runner id; pass their cond ops
  // straight through, else thread_get_mailbox asserts tid_self!=RID_INVALID.
  if (mc_in_wrapper) return libpthread_cond_init(cond, attr);
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
      return libpthread_cond_init(cond, attr);
    case FUZZER_STANDALONE: {
      mc_in_wrapper = 1;
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_INIT_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_wake_scheduler_and_wait();
      int rc = libpthread_cond_init(cond, attr);
      mc_in_wrapper = 0;
      return rc;
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      rec_list *cond_record = get_or_create_object_record(cond, CONDITION_VARIABLE, CV_UNINITIALIZED);

      int rc = libpthread_cond_init(cond, attr);
      if (rc == 0) {
        libpthread_mutex_lock(&cond_record->node_lock);
        cond_record->vo.cond_state.status = CV_INITIALIZED;
        cond_record->vo.cond_state.interacting_thread = 0;
        /* we typically don't know which mutex will be associated with the
           condition variable until a thread actually waits on it. */
        cond_record->vo.cond_state.associated_mutex = NULL;
        cond_record->vo.cond_state.waiting_threads = create_thread_queue();
        cond_record->vo.cond_state.count = 0;
        libpthread_mutex_unlock(&cond_record->node_lock);
        /* Notify deadlock detector that visible progress occurred. */
        deadlock_detector_increment_progress();
      }
      return rc;
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
        volatile runner_mailbox *mb = thread_get_mailbox();
        mb->type = COND_INIT_TYPE;
        memcpy_v(mb->cnts, &cond, sizeof(cond));
        // notify_template_thread();
        // thread_await_scheduler();
        thread_handle_after_dmtcp_restart();
        return libpthread_cond_init(cond, attr);
      }
      case TARGET_BRANCH:
      case TARGET_BRANCH_AFTER_RESTART: {
        volatile runner_mailbox *mb = thread_get_mailbox();
        mb->type = COND_INIT_TYPE;
        memcpy_v(mb->cnts, &cond, sizeof(cond));
        thread_wake_scheduler_and_wait();
        return libpthread_cond_init(cond, attr);
      }
      default: {
        libc_abort();
      }
  }
  return 0;
}

int mc_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  if (mc_in_wrapper) return libpthread_cond_wait(cond, mutex);
  switch (get_current_mode()){
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
      return libpthread_cond_wait(cond, mutex);
    case FUZZER_STANDALONE: {
      // Two-transition protocol (mirrors TARGET_BRANCH): ENQUEUE registers the
      // waiter and releases the mutex; WAIT blocks (in the mailbox) until the
      // scheduler releases it (i.e. a signal/broadcast woke it), then re-acquires
      // the mutex. No real libpthread_cond_wait — the mailbox park replaces it.
      mc_in_wrapper = 1;
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_ENQUEUE_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      memcpy_v(mb->cnts + sizeof(cond), &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      libpthread_mutex_unlock(mutex);
      mb->type = COND_WAIT_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      memcpy_v(mb->cnts + sizeof(cond), &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      libpthread_mutex_lock(mutex);
      mc_in_wrapper = 0;
      return 0;
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      /* Get thread record first (like main branch) */
      pthread_t this_thread = pthread_self();
      pthread_rwlock_rdlock(&rec_list_lock);
      rec_list *thrd_record = find_thread_record_mode(this_thread);
      pthread_rwlock_unlock(&rec_list_lock);
      runner_id_t tmp = thrd_record->vo.thrd_state.id;

      /* Ensure condition variable record exists with CV_INITIALIZED status (like main branch) */
      rec_list *cond_record = get_or_create_object_record(cond, CONDITION_VARIABLE, CV_INITIALIZED);

      /* Register the thread in the outer waiting room (per-node lock replaces global lock) */
      libpthread_mutex_lock(&cond_record->node_lock);
      cond_record->vo.cond_state.interacting_thread = tmp;
      if (!is_in_thread_queue(cond_record->vo.cond_state.waiting_threads, tmp)) {
        enqueue_thread(cond_record->vo.cond_state.waiting_threads, tmp, CV_PREWAITING);
      }
      cond_record->vo.cond_state.associated_mutex = mutex;
      cond_record->vo.cond_state.count++;
      libpthread_mutex_unlock(&cond_record->node_lock);

      struct timespec wait_time = {.tv_sec = 2, .tv_nsec = 0};
      int rc;
      while (1) {
        rc = libpthread_cond_timedwait(cond, mutex, &wait_time);
        if (rc == 0) {
          /* The thread has successfully entered the waiting state. */
          libpthread_mutex_lock(&cond_record->node_lock);
          /* Re-get thread record (like main branch) */
          pthread_rwlock_rdlock(&rec_list_lock);
          thrd_record = find_thread_record_mode(pthread_self());
          pthread_rwlock_unlock(&rec_list_lock);

          /* Check if this thread was signaled (CV_SIGNALED state) */
          condition_variable_status cv_state = get_thread_cv_state(cond_record->vo.cond_state.waiting_threads, thrd_record->vo.thrd_state.id);

          if (cv_state == CV_SIGNALED) {
            /* Remove this thread from the queue */
            remove_thread_from_queue(cond_record->vo.cond_state.waiting_threads, thrd_record->vo.thrd_state.id);
            cond_record->vo.cond_state.count--;
          } else {
            update_thread_cv_state(cond_record->vo.cond_state.waiting_threads, thrd_record->vo.thrd_state.id, CV_WAITING);
          }
          libpthread_mutex_unlock(&cond_record->node_lock);
          /* Notify deadlock detector that progress occurred - thread successfully waited/was signaled */
          deadlock_detector_increment_progress();
          return rc;
        }
        else if (rc == ETIMEDOUT) {
          // Timeout case: The thread did not manage to enter the wait state
          // within the given time frame. In a regular run, this could simply
          // mean the condition was not signaled, but here in model checking
          // mode, it serves a critical purpose:
          //
          // During recording, we do NOT want threads to block indefinitely.
          // Each thread must complete its current visible operation and yield
          // control back to the model checker. Therefore, any thread that times
          // out here (failing to enter the wait state) will check if the model
          // checker requires it to retry. The timeout gives us a way to ensure
          // that threads are not permanently stuck in transition due to an
          // unforeseen checkpoint.
          //
          // After a DMTCP_EVENT_RESTART event, exactly one thread will ultimately
          // succeed in fully acquiring the condition and transitioning to the
          // wait state. Other threads should detect this change and eventually
          // escape from this loop to avoid unnecessary blocking.

          // The purpose of CV_PREWAITING state and Inner/Outer Waiting Room:
          //
          // By setting the state to CV_PREWAITING, we create a two-part "waiting room" mechanism:
          // - The "outer waiting room" corresponds to the transition period right after the thread
          //   has released the mutex but has not fully entered the wait state.
          // - Once the thread enters the full wait state (pthread_cond_wait), it moves into the "inner
          //   waiting room," and its state is set to CV_WAITING. This distinction is critical for
          //   checkpoint safety:
          // - If a checkpoint occurs while the thread is still in the CV_PREWAITING (outer waiting room),
          //   we know it has not fully transitioned to a waiting state and can handle it accordingly.
          // - If the thread is in CV_WAITING (inner waiting room), we know it has entered a stable wait
          //   state, ensuring the mutex-conditional interaction is checkpoint-safe.
          if (is_in_restart_mode()) {
            break;
          }
          /* Check if this thread was signaled while in CV_PREWAITING state */
          libpthread_mutex_lock(&cond_record->node_lock);
          condition_variable_status cv_state = get_thread_cv_state(cond_record->vo.cond_state.waiting_threads, thrd_record->vo.thrd_state.id);
          if (cv_state == CV_SIGNALED) {
            // Thread was signaled while in CV_PREWAITING, break out of loop
            remove_thread_from_queue(cond_record->vo.cond_state.waiting_threads, thrd_record->vo.thrd_state.id);
            cond_record->vo.cond_state.count--;
            libpthread_mutex_unlock(&cond_record->node_lock);
            /* Notify deadlock detector that progress occurred - thread was signaled while in CV_PREWAITING */
            deadlock_detector_increment_progress();
            return 0; // Successful wait (signaled)
          }
          libpthread_mutex_unlock(&cond_record->node_lock);
          /* Notify deadlock detector that progress occurred - thread is actively retrying cond_wait 
           * This prevents false positive deadlock detection during normal producer-consumer patterns */
          deadlock_detector_increment_progress();
        } else if (rc != 0 && rc != ETIMEDOUT) {
          /* A "true" error: pass through to user */
          return rc;
        }
      }
      /* fallthrough to restart handling */
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE:{
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_ENQUEUE_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      memcpy_v(mb->cnts + sizeof(cond), &mutex, sizeof(mutex));
      thread_handle_after_dmtcp_restart();

      /* Release the user mutex before waiting */
      libpthread_mutex_unlock(mutex);

      mb->type = COND_WAIT_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      memcpy_v(mb->cnts + sizeof(cond), &mutex, sizeof(mutex));
      thread_handle_after_dmtcp_restart();

      /* Re-acquire the user mutex after being signaled */
      libpthread_mutex_lock(mutex);
      return 0;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_ENQUEUE_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      memcpy_v(mb->cnts + sizeof(cond), &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();

      libpthread_mutex_unlock(mutex);

      mb->type = COND_WAIT_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      memcpy_v(mb->cnts + sizeof(cond), &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();

      libpthread_mutex_lock(mutex);
      return 0;
    }
    default: {
      libc_abort();
    }
  }
  return 0;
}

int mc_pthread_cond_signal(pthread_cond_t *cond) {
  if (mc_in_wrapper) return libpthread_cond_signal(cond);
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
      return libpthread_cond_signal(cond);
    case FUZZER_STANDALONE: {
      mc_in_wrapper = 1;
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_SIGNAL_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_wake_scheduler_and_wait();
      int rc = libpthread_cond_signal(cond);
      mc_in_wrapper = 0;
      return rc;
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      /* Locate the cond record under read-lock, then use per-node lock */
      pthread_rwlock_rdlock(&rec_list_lock);
      rec_list *cond_record = find_object_record_mode(cond);
      pthread_rwlock_unlock(&rec_list_lock);
      if (cond_record == NULL) {
        fprintf(stderr,
                "Undefined behavior: attempting to signal an uninitialized"
                " condition variable %p\n",
                cond);
        libc_abort();
      }
      /* Count pre-signal waiting threads (only CV_WAITING) */
      libpthread_mutex_lock(&cond_record->node_lock);
      int cv_waiting_count = 0;
      thread_queue_node* current = cond_record->vo.cond_state.waiting_threads->front;
      while (current != NULL) {
        if (current->thread_cv_state == CV_WAITING || current->thread_cv_state == CV_PREWAITING) {
          cv_waiting_count++;
        }
        current = current->next;
      }
      cond_record->vo.cond_state.prev_waiting_count = cv_waiting_count;
      libpthread_mutex_unlock(&cond_record->node_lock);

      int rc = libpthread_cond_signal(cond);
      if (rc == 0) {
        libpthread_mutex_lock(&cond_record->node_lock);
        /* Find a waiting thread and mark it signaled */
        runner_id_t waiting_thread = get_waiting_thread_node(cond_record->vo.cond_state.waiting_threads);
        if (waiting_thread == RID_INVALID && !is_queue_empty(cond_record->vo.cond_state.waiting_threads)) {
          // No CV_WAITING threads found, look for CV_PREWAITING threads
          current = cond_record->vo.cond_state.waiting_threads->front;
          while (current != NULL) {
            if (current->thread_cv_state == CV_PREWAITING) {
              waiting_thread = current->thread;
              break;
            }
            current = current->next;
          }
        }
        if (!is_queue_empty(cond_record->vo.cond_state.waiting_threads)) {
          if (waiting_thread != RID_INVALID) {
            update_thread_cv_state(cond_record->vo.cond_state.waiting_threads, waiting_thread, CV_SIGNALED);
          }
        }

        /* Check whether any thread was marked signaled */
        bool any_thread_signaled = false;
        current = cond_record->vo.cond_state.waiting_threads->front;
        while (current != NULL) {
          if (current->thread_cv_state == CV_SIGNALED) {
            any_thread_signaled = true;
            break;
          }
          current = current->next;
        }

        /* If no thread was signaled but there were waiting threads, it's a lost wakeup */
        if (!any_thread_signaled && cond_record->vo.cond_state.prev_waiting_count > 0) {
          cond_record->vo.cond_state.lost_wakeups++;
          fprintf(stderr, "WARNING: Lost wakeup detected on condition variable %p\n", cond);
        }

        libpthread_mutex_unlock(&cond_record->node_lock);
        /* Notify deadlock detector that visible progress occurred. */
        deadlock_detector_increment_progress();
      }
      return rc;
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_SIGNAL_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_handle_after_dmtcp_restart();
      return libpthread_cond_signal(cond);
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_SIGNAL_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_wake_scheduler_and_wait();
      return libpthread_cond_signal(cond);
    }
    default: {
      libc_abort();
    }
  }
  return 0;
}

int mc_pthread_cond_broadcast(pthread_cond_t *cond) {
  if (mc_in_wrapper) return libpthread_cond_broadcast(cond);
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
      return libpthread_cond_broadcast(cond);
    case FUZZER_STANDALONE: {
      mc_in_wrapper = 1;
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_BROADCAST_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_wake_scheduler_and_wait();
      int rc = libpthread_cond_broadcast(cond);
      mc_in_wrapper = 0;
      return rc;
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      /* Locate the cond record under read-lock, then use per-node lock */
      pthread_rwlock_rdlock(&rec_list_lock);
      rec_list *cond_record = find_object_record_mode(cond);
      pthread_rwlock_unlock(&rec_list_lock);
      if (cond_record == NULL) {
        fprintf(stderr,
                "Undefined behavior: attempting to broadcast an uninitialized"
                " condition variable %p\n",
                cond);
        libc_abort();
      }

      int rc = libpthread_cond_broadcast(cond);
      if (rc == 0) {
        libpthread_mutex_lock(&cond_record->node_lock);
        /* Mark all waiting threads as signaled */
        thread_queue_node* current = cond_record->vo.cond_state.waiting_threads->front;
        while (current != NULL) {
          /* Only mark CV_WAITING threads as signaled (not transitional) */
          if (current->thread_cv_state == CV_WAITING) {
            update_thread_cv_state(cond_record->vo.cond_state.waiting_threads,
                                   current->thread, CV_SIGNALED);
          }
          current = current->next;
        }
        libpthread_mutex_unlock(&cond_record->node_lock);
        /* Notify deadlock detector that visible progress occurred. */
        deadlock_detector_increment_progress();
      }
      return rc;
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_BROADCAST_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_handle_after_dmtcp_restart();
      return libpthread_cond_broadcast(cond);
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_BROADCAST_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_wake_scheduler_and_wait();
      return libpthread_cond_broadcast(cond);
    }
    default: {
      libc_abort();
    }
  }
  return 0;
}

int mc_pthread_cond_destroy(pthread_cond_t *cond) {
  if (mc_in_wrapper) return libpthread_cond_destroy(cond);
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
      return libpthread_cond_destroy(cond);
    case FUZZER_STANDALONE: {
      mc_in_wrapper = 1;
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_DESTROY_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_wake_scheduler_and_wait();
      int rc = libpthread_cond_destroy(cond);
      mc_in_wrapper = 0;
      return rc;
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      /* Locate the cond record under read-lock, then use per-node lock */
      pthread_rwlock_rdlock(&rec_list_lock);
      rec_list *cond_record = find_object_record_mode(cond);
      pthread_rwlock_unlock(&rec_list_lock);
      if (cond_record == NULL) {
        fprintf(stderr,
                "Undefined behavior: attempting to destroy an uninitialized"
                " condition variable %p\n",
                cond);
        libc_abort();
      }

      /* Check if any thread is waiting on this condition variable */
      libpthread_mutex_lock(&cond_record->node_lock);
      if (!is_queue_empty(cond_record->vo.cond_state.waiting_threads)) {
        libpthread_mutex_unlock(&cond_record->node_lock);
        fprintf(stderr,
                "Undefined behavior: attempting to destroy a condition variable"
                " that has threads waiting on it %p\n",
                cond);
        libc_abort();
      }
      libpthread_mutex_unlock(&cond_record->node_lock);

      int rc = libpthread_cond_destroy(cond);
      if (rc == 0) {
        libpthread_mutex_lock(&cond_record->node_lock);
        cond_record->vo.cond_state.status = CV_DESTROYED;
        libpthread_mutex_unlock(&cond_record->node_lock);
        /* Notify deadlock detector that visible progress occurred. */
        deadlock_detector_increment_progress();
      }
      return rc;
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_DESTROY_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_handle_after_dmtcp_restart();
      return libpthread_cond_destroy(cond);
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_DESTROY_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_wake_scheduler_and_wait();
      return libpthread_cond_destroy(cond);
    }
    default: {
      libc_abort();
    }
  }
  return 0;
}
