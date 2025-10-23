#define _GNU_SOURCE
#include <assert.h>
#include <dmtcp.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "mcmini/common/exit.h"
#include "mcmini/Thread_queue.h"
#include "mcmini/mcmini.h"
#include "deadlock_detector.h"

// TSan's public annotation API (see <sanitizer/tsan_interface.h>): same
// idiom as log_mut in log.c. The real lock/unlock here bypasses TSan's
// interceptor entirely (mc_load_intercepted_pthread_functions() in
// interception.c), causing a false race on every mutex-protected variable
// (e.g. cv-producer-consumer's count/buffer) without this annotation.
extern void __tsan_acquire(void *addr) __attribute__((weak));
extern void __tsan_release(void *addr) __attribute__((weak));

// TSan's Fiber API (weak: no-ops for non-TSan targets), used by
// mc_pthread_exit() to give a DMTCP-resurrected thread a fresh, empty TSan
// shadow call stack before terminating it via a raw exit syscall -- see
// that function and mc_pthread_is_recreated_thread()'s doc comment in
// wrappers.h. Mirrors the same idiom already used in dmtcp-callback.c for
// resurrected/forking threads' first resumption.
extern void *__tsan_create_fiber(unsigned flags) __attribute__((weak));
extern void __tsan_switch_to_fiber(void *fiber, unsigned flags)
    __attribute__((weak));

typedef struct pthread_map {
    pthread_t thread;
    runner_id_t value;
    // Posted by whichever thread eventually calls pthread_join() on this
    // one (mc_pthread_join()'s TARGET_BRANCH*/DMTCP_RESTART_INTO_BRANCH-ish
    // cases), waited on by this thread itself before it may really
    // terminate (mc_exit_thread_in_child()). See the two functions for the
    // full rationale.
    sem_t exit_permission_sem;
    // Posted by this thread right after exit_permission_sem, immediately
    // before it actually finishes. mc_pthread_join() waits on this instead
    // of performing a real join for threads a real join could never
    // resolve for anyway (see registered_post_restart below). Posting it
    // unconditionally is harmless on paths where nothing waits on it.
    sem_t really_exited_sem;
    // Whether this thread's own (one-time) registration happened while
    // get_current_mode() was already TARGET_BRANCH_AFTER_RESTART -- i.e.
    // whether it was created via a real pthread_create() call made after a
    // restart, as opposed to being one of the threads DMTCP resurrected via
    // clone() (which never re-registers: such a thread resumes mid-
    // execution via setcontext(), so its only-ever registration is from the
    // original, pre-restart RECORD-mode run). A clone()-recreated thread
    // never went through TSan's own pthread_create() interceptor, so TSan
    // has no Tid for it and a real pthread_join() on it can never resolve
    // (confirmed against TSan's own source -- see TSAN-pthread-join.md).
    // mc_pthread_join() uses this flag to decide whether a real join is
    // even possible for a given target thread.
    bool registered_post_restart;
    struct pthread_map *next;
} pthread_map_t;

// NOTE: This lock and allocator must be TSan-safe: insert_pthread_map runs from
// mc_thread_routine_wrapper's prologue, which under DMTCP executes before
// libtsan has registered the thread. So, we use libmcmini's libpthread_* handle
// wrappers (which bypass libtsan) rather than the raw pthread_rwlock_* symbols
// (which resolve to libtsan's interceptors), and mc_ts_alloc rather than malloc.
// See TSAN-McMini-DMTCP.txt.
static pthread_mutex_t pthread_map_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_map_t *head = NULL;

void insert_pthread_map(pthread_t t, runner_id_t v) {
    libpthread_mutex_lock(&pthread_map_lock);
    pthread_map_t *n = mc_ts_alloc(sizeof *n);
    n->thread = t;
    n->value = v;
    libpthread_sem_init(&n->exit_permission_sem, 0, 0);
    libpthread_sem_init(&n->really_exited_sem, 0, 0);
    n->registered_post_restart = (get_current_mode() == TARGET_BRANCH_AFTER_RESTART);
    n->next = head;
    head = n;
    libpthread_mutex_unlock(&pthread_map_lock);
}

// Returns the given thread's own pthread_map_t entry, or NULL if the thread
// is not (yet) registered. The returned pointer stays valid to use after
// unlocking: entries are only ever appended, never removed or reallocated.
static pthread_map_t *find_pthread_map_entry(pthread_t t) {
    libpthread_mutex_lock(&pthread_map_lock);
    pthread_map_t *result = NULL;
    for (pthread_map_t *cur = head; cur != NULL; cur = cur->next) {
        if (pthread_equal(cur->thread, t)) {
            result = cur;
            break;
        }
    }
    libpthread_mutex_unlock(&pthread_map_lock);
    return result;
}

runner_id_t search_pthread_map(pthread_t t) {
    pthread_map_t *entry = find_pthread_map_entry(t);
    return entry == NULL ? RID_INVALID : entry->value;
}

// Returns the given thread's own exit_permission_sem (see pthread_map_t),
// or NULL if the thread is not (yet) registered.
sem_t *find_exit_permission_sem(pthread_t t) {
    pthread_map_t *entry = find_pthread_map_entry(t);
    return entry == NULL ? NULL : &entry->exit_permission_sem;
}

// Returns the given thread's own really_exited_sem (see pthread_map_t), or
// NULL if the thread is not (yet) registered.
sem_t *find_really_exited_sem(pthread_t t) {
    pthread_map_t *entry = find_pthread_map_entry(t);
    return entry == NULL ? NULL : &entry->really_exited_sem;
}


MCMINI_THREAD_LOCAL runner_id_t tid_self = RID_INVALID;

// Set (on the creating thread) while libmcmini creates one of its OWN helper
// threads (e.g. the template thread) via the public pthread_create. It tells
// mc_pthread_create to create the thread plainly -- routed through DMTCP and
// visible to any sanitizer's pthread_create interceptor -- rather than treating
// it as a user thread to be model-checked. Keeping it visible to
// ThreadSanitizer is what lets its ThreadState round-trip checkpoint/restart.
// See TSAN-McMini-DMTCP.txt.
MCMINI_THREAD_LOCAL int mc_creating_internal_thread = 0;

runner_id_t mc_register_this_thread(void) {
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
  if (attr != NULL) {
    int type;
    pthread_mutexattr_gettype(attr, &type);
    assert(type == PTHREAD_MUTEX_NORMAL);
  }

  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case EXTERNAL_THREAD: {
      return libpthread_mutex_init(mutex, attr);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      // NOTE: This is subtle: at this point, the possible modes are
      // RECORD ***AND*** DMTCP_RESTART. The latter is possible if
      // checkpointing occurs anywhere AFTER the switch statement above.
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *mutex_record = find_object_record_mode(mutex);
      if (mutex_record == NULL) {
        // FIXME: We assume that this is a normal mutex. For other mutex
        // types, we'd need to behave differently
        visible_object vo = {
            .type = MUTEX, .location = mutex,
            .mut_state = {.status = UNINITIALIZED, .owner = RID_INVALID}};
        mutex_record = add_rec_entry_record_mode(&vo);
      }
      libpthread_mutex_unlock(&rec_list_lock);

      int rc = libpthread_mutex_init(mutex, attr);
      if (rc == 0) {  // Init
        libpthread_mutex_lock(&rec_list_lock);
        mutex_record->vo.mut_state.status = UNLOCKED;
        libpthread_mutex_unlock(&rec_list_lock);
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
    case EXTERNAL_THREAD: {
      int rc = libpthread_mutex_lock(mutex);
      if (rc == 0 && __tsan_acquire) __tsan_acquire(mutex);
      return rc;
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      // NOTE: This is subtle: at this point, the possible modes are
      // RECORD ***AND*** DMTCP_RESTART. The latter is possible if
      // checkpointing occurs anywhere AFTER the switch statement above.
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *mutex_record = find_object_record_mode(mutex);
      if (mutex_record == NULL) {
        visible_object vo = {
            .type = MUTEX, .location = mutex,
            .mut_state = {.status = UNINITIALIZED, .owner = RID_INVALID}};
        mutex_record = add_rec_entry_record_mode(&vo);
      }
      libpthread_mutex_unlock(&rec_list_lock);

      while (1) {
        // Recompute the deadline every iteration -- see mc_sem_wait()'s
        // identical pattern in sem-wrappers.c. A hoisted, one-time-computed
        // {.tv_sec = 2} here would be interpreted as an absolute deadline of
        // 2 seconds past the epoch, already long past, making every
        // subsequent call return ETIMEDOUT immediately instead of ever
        // genuinely blocking.
        struct timespec time;
        clock_gettime(CLOCK_REALTIME, &time);
        time.tv_sec += 2;
        int rc = libpthread_mutex_timedlock(mutex, &time);
        if (rc == 0) {  // Lock succeeded
          libpthread_mutex_lock(&rec_list_lock);
          // Record the owner too: a checkpoint can land while this thread is
          // mid pthread_cond_wait(), still modeled as the mutex's holder
          // until the enqueue transition runs (condition_variable_enqueue_
          // thread::modify() requires mutex->is_locked_by(executor)).
          mutex_record->vo.mut_state.status = LOCKED;
          mutex_record->vo.mut_state.owner = tid_self;
          libpthread_mutex_unlock(&rec_list_lock);
          if (__tsan_acquire) __tsan_acquire(mutex);
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
      int rc = libpthread_mutex_lock(mutex);
      if (rc == 0 && __tsan_acquire) __tsan_acquire(mutex);
      return rc;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_LOCK_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      int rc = libpthread_mutex_lock(mutex);
      if (rc == 0 && __tsan_acquire) __tsan_acquire(mutex);
      return rc;
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
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case EXTERNAL_THREAD: {
      if (__tsan_release) __tsan_release(mutex);
      return libpthread_mutex_unlock(mutex);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *mutex_record = find_object_record_mode(mutex);
      if (mutex_record == NULL) {
        // FIXME: We assume that this is a normal mutex. For other mutex
        // types, we'd need to behave differently
        fprintf(stderr,
                "Undefined behavior: attempting to unlock an uninitialized "
                "mutex %p",
                mutex);
        libc_abort();
      }
      libpthread_mutex_unlock(&rec_list_lock);
      if (__tsan_release) __tsan_release(mutex);
      int rc = libpthread_mutex_unlock(mutex);
      if (rc == 0) {  // Unlock succeeded
        libpthread_mutex_lock(&rec_list_lock);
        mutex_record->vo.mut_state.status = UNLOCKED;
        mutex_record->vo.mut_state.owner = RID_INVALID;
        libpthread_mutex_unlock(&rec_list_lock);
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
      if (__tsan_release) __tsan_release(mutex);
      return libpthread_mutex_unlock(mutex);
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = MUTEX_UNLOCK_TYPE;
      memcpy_v(mb->cnts, &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      if (__tsan_release) __tsan_release(mutex);
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

  // Wait for whichever thread eventually calls pthread_join() on this one
  // (see mc_pthread_join()'s TARGET_BRANCH*/DMTCP_RESTART_INTO_BRANCH-ish
  // cases) to grant permission before this thread is allowed to really
  // terminate. This keeps this thread's pthread_t/tid valid for exactly as
  // long as a real, unjoined POSIX thread's would be -- no longer --
  // rather than parking it forever regardless of whether anyone ever
  // joins it.
  sem_t *exit_permission = find_exit_permission_sem(pthread_self());
  assert(exit_permission != NULL);
  libpthread_sem_wait(exit_permission);

  // Signal genuine completion, in case the joiner can't use a real join
  // (see mc_pthread_join()'s TARGET_BRANCH_AFTER_RESTART case).
  sem_t *really_exited = find_really_exited_sem(pthread_self());
  assert(really_exited != NULL);
  libpthread_sem_post(really_exited);
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
      // Unlike every other wrapper's TARGET_BRANCH* case, this used to call
      // the wait-only thread_await_scheduler(), which never posts
      // model_side_sem. That's harmless for a thread that fell through from
      // DMTCP_RESTART_INTO_BRANCH/TEMPLATE above (thread_handle_after_dmtcp_restart()
      // already posted it), but a thread already in TARGET_BRANCH_AFTER_RESTART
      // (e.g. main(), calling exit() for the first time since restart) jumps
      // straight here and hangs the coordinator's execute_runner() forever.
      thread_wake_scheduler_and_wait();

      // After "exiting", don't actually exit yet:
      // the model checker will prevent the process
      // from continuing execution and will note this
      // branch as "useless" since at this point
      mb->type = PROCESS_EXIT_TYPE;
      memcpy_v(mb->cnts, &status, sizeof(status));
      thread_wake_scheduler_and_wait();
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

MCMINI_NO_RETURN void mc_pthread_exit(void *retval) {
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case EXTERNAL_THREAD:
    case RECORD:
    case PRE_CHECKPOINT: {
      libpthread_pthread_exit(retval);
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      thread_get_mailbox()->type = THREAD_EXIT_TYPE;
      thread_handle_after_dmtcp_restart();
      // Fallthrough
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      if (tid_self == RID_MAIN_THREAD) {
        mc_exit_main_thread_in_child();  // never returns
      } else {
        mc_exit_thread_in_child();
      }
      // mc_exit_thread_in_child() only returns once safe to really
      // terminate -- unlike returning from a start routine, pthread_exit()
      // must never return to its caller, so do that for real here. A
      // DMTCP-resurrected thread runs on a TSan fiber and crashes in
      // glibc's real pthread_exit() (see doc/pthread-exit-abort-and-fiber-
      // crash.txt); any other thread gets a real pthread_exit() as usual.
      if (mc_pthread_is_recreated_thread(pthread_self())) {
        if (__tsan_switch_to_fiber) {
          __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
        }
        syscall(SYS_exit, 0);
        libc_abort();  // unreachable
      }
      libpthread_pthread_exit(retval);
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
    case EXTERNAL_THREAD: {
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
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *thread_record = find_thread_record_mode(this_thread);
      assert(thread_record == NULL);
      visible_object vo = {.type = THREAD,
                           .location = NULL,
                           .thrd_state.pthread_desc = this_thread,
                           .thrd_state.status = ALIVE,
                           .thrd_state.id = rid};
      // TSan-safe allocation: under DMTCP this runs before libtsan has
      // registered the thread (see TSAN-McMini-DMTCP.txt).
      thread_record = add_rec_entry_record_mode_ts(&vo);
      libpthread_mutex_unlock(&rec_list_lock);
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
    default: {
      libc_abort();
    }
  }
  void *rv = unwrapped_arg->routine(unwrapped_arg->arg);
  free(arg);

  switch (get_current_mode()) {
    case RECORD:
    case PRE_CHECKPOINT: {
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *thread_record = find_thread_record_mode(pthread_self());
      assert(thread_record != NULL);
      thread_record->vo.thrd_state.status = EXITED;
      fprintf(stdout, "\n\nexited\n\n"); fflush(stdout);
      libpthread_mutex_unlock(&rec_list_lock);
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
  libpthread_mutex_lock(&rec_list_lock);
  rec_list *thread_record = find_thread_record_mode(main_thread);
  assert(thread_record == NULL);
  visible_object vo = {.type = THREAD,
                       .location = NULL,
                       .thrd_state.id = main_tid,
                       .thrd_state.pthread_desc = main_thread,
                       .thrd_state.status = ALIVE};
  thread_record = add_rec_entry_record_mode(&vo);
  libpthread_mutex_unlock(&rec_list_lock);
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

// Set (per-thread) by dmtcp_create_checkpoint_thread_wrapper() below, and
// resolved lazily by resolve_ckpt_window_candidate_if_pending() (called from
// get_current_mode()) on this thread's own first wrapped call.
static MCMINI_THREAD_LOCAL bool is_ckpt_window_candidate = false;
static MCMINI_THREAD_LOCAL int ckpt_window_candidate_virtual_tid;

// Both the real checkpoint thread and TSAN's nested background-thread spawn
// (see dmtcp_create_checkpoint_thread_wrapper()'s comment) reach the same
// wrapper, and neither can tell, at creation time, which one it is. We can't
// resolve that here by calling dmtcp_tsan_background_thread_virtual_tid()
// and waiting: classification requires DMTCP's endCkptThreadCreationWindow(),
// which does not run until DMTCP's createCkptThread() sees its own
// pthread_create() call return -- which itself requires this thread to be
// registered with TSan, which happens only once this thread runs its real
// routine (below). Blocking here to wait for classification would therefore
// deadlock.
//
// So instead: mark this thread as a candidate and return immediately,
// letting it proceed into its real routine. Classification then happens
// lazily, on this same thread's first subsequent wrapped call (necessarily
// after TSan has registered it, so no longer deadlock-prone).
void mark_ckpt_window_candidate(int virtual_tid) {
  ckpt_window_candidate_virtual_tid = virtual_tid;
  is_ckpt_window_candidate = true;
}

void resolve_ckpt_window_candidate_if_pending(void) {
  if (!is_ckpt_window_candidate) return;
  is_ckpt_window_candidate = false;

  int tsan_bg_virtual_tid = dmtcp_tsan_background_thread_virtual_tid();
  while (tsan_bg_virtual_tid == -1) {
    sched_yield();
    tsan_bg_virtual_tid = dmtcp_tsan_background_thread_virtual_tid();
  }
  if (tsan_bg_virtual_tid != ckpt_window_candidate_virtual_tid) {
    record_checkpoint_thread();
  }
  // else: this is TSAN's own background thread (tsan_bg_virtual_tid ==
  // ckpt_window_candidate_virtual_tid), not the checkpoint thread -- no
  // checkpoint-thread bookkeeping needed.
}

void *dmtcp_create_checkpoint_thread_wrapper(void *arg) {
  struct mc_thread_routine_arg *unwrapped_arg = arg;

  // Unblock mc_pthread_create()'s caller immediately; see
  // mark_ckpt_window_candidate()'s comment for why classification can't
  // happen here, before running the real routine.
  //
  // Trade-off: this leaves a window, from here until this thread's first
  // subsequent wrapped call, during which ckpt_pthread_descriptor is not
  // yet set (previously guaranteed set by the time createCkptThread()
  // returned).
  //
  // This thread may be TSAN's own background thread, nested-spawned from
  // inside TSAN's own pthread_create interceptor while it was still handling
  // the real checkpoint thread's creation request, and such a thread never
  // goes through TSAN's own thread registration -- so it has no TSAN
  // ThreadState of its own. That's safe here: mc_pthread_create()'s
  // PRE_CHECKPOINT_THREAD case already forced libmcmini_init() to complete
  // on the creating thread before spawning this one, so libmcmini_init()'s
  // own fast path (see interception.c) means this call never reaches
  // pthread_once()/TSAN's interceptor at all.
  libpthread_sem_post(&unwrapped_arg->mc_pthread_create_binary_sem);

  mark_ckpt_window_candidate((int)syscall(SYS_gettid));

  void *rv = unwrapped_arg->routine(unwrapped_arg->arg);
  free(arg);
  return rv;
}

int mc_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*routine)(void *), void *arg) {
  // NOTE: We're assuming that DMTCP creates only the checkpoint thread
  // immediately after sending the `DMTCP_EVENT_INIT` to `libmcmini.so`
  // and creates no other threads during execution
  static pthread_once_t main_thread_once = PTHREAD_ONCE_INIT;

  // libmcmini's own helper thread (e.g. the template thread). It reached us
  // through the sanitizer's pthread_create interceptor (if any) -- so TSAN has
  // already registered it and wrapped `routine` -- and we now just hand it to
  // DMTCP, so it is DMTCP-known, with no user-thread machinery. Creating it via
  // the public pthread_create (rather than libdmtcp_pthread_create directly)
  // is deliberate: it keeps the thread visible to libtsan, so its ThreadState
  // survives checkpoint/restart. See TSAN-McMini-DMTCP.txt.
  if (mc_creating_internal_thread) {
    return libdmtcp_pthread_create(thread, attr, routine, arg);
  }

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

      // This call may be the real checkpoint thread's own creation, or a
      // nested spawn of TSAN's background thread happening as a side effect
      // of it (see dmtcp_create_checkpoint_thread_wrapper()'s own comment).
      // Either way, the new thread figures out which one it is for itself,
      // once running; we just need to synchronize enough to know it has
      // started before returning here.
      //
      // Force libmcmini_init()'s lazy resolution to complete HERE, on this
      // (already TSAN-registered) thread, before the new thread is spawned.
      // The new thread may turn out to be TSAN's own background thread,
      // which never gets its own TSAN ThreadState (see
      // dmtcp_create_checkpoint_thread_wrapper()) -- calling libmcmini_init()
      // there would reach pthread_once(), which TSAN's own interceptor
      // intercepts and crashes on for exactly that reason. pthread_create()
      // below is itself a synchronization point, so completing the
      // resolution here guarantees it's visible on the new thread without
      // it ever needing to call libmcmini_init() itself.
      libmcmini_init();

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
    case EXTERNAL_THREAD: {
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
      // Force libmcmini_init()'s lazy resolution to complete HERE, on this
      // (already TSAN-registered) thread, before the new thread is spawned.
      // Under DMTCP, the new thread's own prologue (mc_thread_routine_wrapper
      // -> mc_register_this_thread(), wrappers.c) runs before TSAN's own
      // registration trampoline does (see TSAN-McMini-DMTCP.txt), so it has
      // no TSAN ThreadState yet when it makes its own first wrapped call.
      // pthread_create() below is itself a synchronization point, so
      // completing the resolution here guarantees libmcmini_init()'s fast
      // path (see interception.c) applies on the new thread, and it never
      // needs to reach pthread_once()/TSAN's interceptor itself.
      libmcmini_init();

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
      //
      // Use tsan_or_real_pthread_create(), NOT libpthread_pthread_create():
      // the latter always resolves straight to real glibc, bypassing TSan's
      // own pthread_create interceptor entirely, so this thread never gets a
      // TSan ThreadState. In classic mode that's a live landmine: this very
      // thread's own first call into a libpthread_* wrapper triggers
      // libmcmini_init()'s pthread_once(), which TSan also intercepts, and
      // TSan dereferences this thread's (nonexistent) ThreadState and SEGVs.
      // See doc/classic-mode-thread-registration-segv.txt.
      const int rv = tsan_or_real_pthread_create(
          thread, attr, &mc_thread_routine_wrapper,
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
    default: {
      libc_abort();
    }
  }
}

// Shared implementation for mc_pthread_join() and mc_pthread_join_maybe_defer().
//
// If defer_real_join is false, behaves exactly as a normal join wrapper:
// whenever a real join is warranted, it performs it directly (via
// libpthread_pthread_join(), which bypasses TSan's own interceptor) and
// *deferred is set to false.
//
// If defer_real_join is true, then whenever a real join is warranted, this
// function does everything BUT the actual join call -- model bookkeeping,
// granting exit permission -- and sets *deferred to true, returning a
// value the caller must ignore. The caller (see __wrap_pthread_join(), in
// pthread_join_wrap.c, linked directly into the target alongside
// `-Wl,--wrap=pthread_join`) must then call __real_pthread_join() itself
// and return its result. This exists because __real_pthread_join is only
// a valid symbol within the target's own -Wl,--wrap=pthread_join link;
// libmcmini.so (a separate, already-built shared library) cannot call it
// directly. Doing the real join this way -- through TSan's own
// interceptor, via __real_pthread_join, rather than bypassing it via
// libpthread_pthread_join() -- lets TSan establish its own happens-before
// edge for this join, instead of never seeing it at all.
static int mc_pthread_join_impl(pthread_t t, void **rv, bool defer_real_join,
                                bool *deferred) {
  *deferred = false;
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT: {
      // This case implies that DMTCP attempted to join
      // a thread BEFORE the `DMTCP_EVENT_INIT` was delivered
      // to `libmcmini`'s callback
      // NOTE: Explicit fallthrough intended
      assert(0);
    }
    case PRE_CHECKPOINT_THREAD:
    case EXTERNAL_THREAD: {
      return libdmtcp_pthread_join(t, rv);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      // NOTE: This is subtle: at this point, the possible modes are
      // RECORD ***AND*** DMTCP_RESTART. The latter is possible if
      // checkpointing occurs anywhere AFTER the switch statement above.
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *thread_record = find_thread_record_mode(t);
      assert(thread_record != NULL);
      libpthread_mutex_unlock(&rec_list_lock);

      while (1) {
        // Recompute the deadline every iteration -- see mc_sem_wait()'s
        // identical pattern in sem-wrappers.c and mc_pthread_mutex_lock()
        // above; a hoisted, one-time {.tv_sec = 2} is an already-past
        // absolute deadline, not "2 seconds from now".
        struct timespec time;
        clock_gettime(CLOCK_REALTIME, &time);
        time.tv_sec += 2;
        // Use the libtsan-bypassing handle: a direct pthread_timedjoin_np would
        // hit libtsan's interceptor and trip its thread-registry CHECK under
        // DMTCP. See TSAN-McMini-DMTCP.txt.
        //
        // FIXME: bypassing TSAN's interceptor here means TSAN never sees this
        // join, so at exit it reports "ThreadSanitizer: thread leak" for
        // every thread joined this way, even though the real join above
        // genuinely succeeded and reaped the thread. TSAN labels this a
        // WARNING (not "ERROR: ThreadSanitizer: ..."), but it still counts
        // toward TSAN's report tally and triggers TSAN's default nonzero
        // exitcode (66) same as any other detected issue. If TSAN_OPTIONS
        // includes halt_on_error:true (or halt_on_error=1), TSAN treats ANY
        // detected issue -- this leak included -- as fatal and can exit
        // immediately at the point of detection rather than letting the
        // program run to completion. Accepted trade-off for now: the
        // registry CHECK this bypass avoids is a hard abort, strictly worse
        // than a leak report. A TSAN suppressions file line of the form
        // "thread:PATH_TO/libmcmini.so" may suppress this specific leak
        // report if it becomes a problem (not yet tried/verified here).
        // "called_from_lib:libmcmini.so" is an alternate suppression line
        // that may work instead (also not yet tried/verified).
        int rc = libpthread_timedjoin_np(t, rv, &time);
        if (rc == 0) {  // Join succeeded
          libpthread_mutex_lock(&rec_list_lock);
          thread_record->vo.thrd_state.status = EXITED;
          libpthread_mutex_unlock(&rec_list_lock);
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
    case TARGET_BRANCH: {
      // Never restarted: the target can only be a thread created via a
      // real pthread_create(), so it always has a valid TSan Tid and a
      // real join always works.
      runner_id_t rid = search_pthread_map(t);
      memcpy_v(thread_get_mailbox()->cnts, &rid, sizeof(runner_id_t));
      thread_get_mailbox()->type = THREAD_JOIN_TYPE;
      thread_wake_scheduler_and_wait();

      // Grant the target thread permission to really terminate now that
      // it's been joined (see mc_exit_thread_in_child()), then perform a
      // genuine join, so its OS thread actually dies -- and *rv is
      // genuinely populated -- before we return, instead of only
      // simulating success at the model level.
      sem_t *exit_permission = find_exit_permission_sem(t);
      assert(exit_permission != NULL);
      libpthread_sem_post(exit_permission);
      if (defer_real_join) {
        *deferred = true;
        return 0;
      }
      return libpthread_pthread_join(t, rv);
    }
    case TARGET_BRANCH_AFTER_RESTART: {
      runner_id_t rid = search_pthread_map(t);
      memcpy_v(thread_get_mailbox()->cnts, &rid, sizeof(runner_id_t));
      thread_get_mailbox()->type = THREAD_JOIN_TYPE;
      thread_wake_scheduler_and_wait();

      // Grant the target thread permission to really terminate now that
      // it's been joined (see mc_exit_thread_in_child()).
      sem_t *exit_permission = find_exit_permission_sem(t);
      assert(exit_permission != NULL);
      libpthread_sem_post(exit_permission);

      pthread_map_t *target = find_pthread_map_entry(t);
      assert(target != NULL);
      if (target->registered_post_restart) {
        // Created fresh after the restart via a real pthread_create():
        // has a valid TSan Tid, so a real join works normally.
        if (defer_real_join) {
          *deferred = true;
          return 0;
        }
        return libpthread_pthread_join(t, rv);
      }
      if (!mc_pthread_is_recreated_thread(t)) {
        // Never recreated: it already exited during RECORD mode, before
        // any checkpoint. really_exited_sem is only posted post-restart,
        // so waiting here would block forever -- the model already knows
        // it's exited (see translate_recorded_runner_to_model()).
        return 0;
      }
      // This thread predates the restart AND was recreated by DMTCP via
      // clone(), bypassing TSan's own pthread_create() interceptor
      // entirely. TSan has no Tid for it, so a real pthread_join() on it
      // can never resolve (see TSAN-pthread-join.md). Wait for its own
      // signal of genuine completion instead of joining it for real.
      libpthread_sem_wait(&target->really_exited_sem);
      return 0;
    }
    default: {
      libc_abort();
    }
  }

}

int mc_pthread_join(pthread_t t, void **rv) {
  bool deferred;
  return mc_pthread_join_impl(t, rv, false, &deferred);
}

int mc_pthread_join_maybe_defer(pthread_t t, void **rv, bool *deferred) {
  return mc_pthread_join_impl(t, rv, true, deferred);
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
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD: {
      return libpthread_cond_init(cond, attr);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *cond_record = find_object_record_mode(cond);
      if (cond_record == NULL) {
        //Initialize the condition variable
        visible_object vo = {
            .type = CONDITION_VARIABLE, .location = cond,
            .cond_state = { .status = CV_UNINITIALIZED, .interacting_thread = 0, .associated_mutex = NULL,
            .count = 0 }
        };
        cond_record = add_rec_entry_record_mode(&vo);
      }
      libpthread_mutex_unlock(&rec_list_lock);

      int rc = libpthread_cond_init(cond, attr);
      if (rc == 0) {
        libpthread_mutex_lock(&rec_list_lock);
        cond_record->vo.cond_state.status = CV_INITIALIZED;
        cond_record->vo.cond_state.interacting_thread = 0;
        //we typically don't know which mutex will be associated with the
        //condition variable until a thread actually waits on it.
        cond_record->vo.cond_state.associated_mutex = NULL;
        cond_record->vo.cond_state.waiting_threads = create_thread_queue();
        cond_record->vo.cond_state.count = 0;
        libpthread_mutex_unlock(&rec_list_lock);
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
        // Do NOT call the real libpthread_cond_init() here: see
        // doc/glibc-cond-var-desync.txt. Past this point mc_pthread_cond_wait()
        // never touches the real cond_t again either, so reinitializing it
        // would only risk racing a clone()-resurrected thread that is still
        // genuinely blocked inside a pre-restart real pthread_cond_timedwait()
        // call, for no benefit.
        return 0;
      }
      case TARGET_BRANCH:
      case TARGET_BRANCH_AFTER_RESTART: {
        volatile runner_mailbox *mb = thread_get_mailbox();
        mb->type = COND_INIT_TYPE;
        memcpy_v(mb->cnts, &cond, sizeof(cond));
        thread_wake_scheduler_and_wait();
        // See the DMTCP_RESTART_INTO_BRANCH/TEMPLATE case above: no real
        // libpthread_cond_init() call here either.
        return 0;
      }
      default: {
        libc_abort();
      }
  }
}

int mc_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  switch (get_current_mode()){
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD: {
      return libpthread_cond_wait(cond, mutex);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      pthread_t this_thread = pthread_self();
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *thrd_record = find_thread_record_mode(this_thread);
      rec_list *cond_record = find_object_record_mode(cond);
      runner_id_t tmp = thrd_record->vo.thrd_state.id;
      if (cond_record == NULL) {
      //Initialize the condition variable
        visible_object vo = {
          .type = CONDITION_VARIABLE, .location = cond, .cond_state = { .status= CV_INITIALIZED, .interacting_thread = tmp,
          .associated_mutex = mutex, .count = 0, .waiting_threads = create_thread_queue() }
        };
        cond_record = add_rec_entry_record_mode(&vo);
      }
      // The thread will enter in the outer waiting room first. Here its state will be
      // CV_PREWAITING. It is done to avoid race condition that might occur due to checkpointing
      // between releasing the mutex and actually getting into wait state.
      cond_record->vo.cond_state.interacting_thread = tmp;
      //check if thread is not already in the waiting room
      if (!is_in_thread_queue(cond_record->vo.cond_state.waiting_threads, tmp)) {
        //add the thread to the waiting room
      enqueue_thread(cond_record->vo.cond_state.waiting_threads,tmp,CV_PREWAITING);
      }
      cond_record->vo.cond_state.associated_mutex = mutex;
      cond_record->vo.cond_state.count++;
      libpthread_mutex_unlock(&rec_list_lock);

      int rc;
      while (1) {
        // Recompute the deadline every iteration -- see mc_sem_wait()'s
        // identical pattern in sem-wrappers.c; a hoisted, one-time
        // {.tv_sec = 2} is an already-past absolute deadline, not "2
        // seconds from now", so every call would return ETIMEDOUT
        // immediately instead of ever genuinely blocking.
        struct timespec wait_time;
        clock_gettime(CLOCK_REALTIME, &wait_time);
        wait_time.tv_sec += 2;
        // pthread_cond_timedwait() really unlocks/relocks mutex every
        // iteration, even on ETIMEDOUT; same interceptor bypass as the
        // TARGET_BRANCH/DMTCP_RESTART_INTO_* cases below, needing the
        // same annotation here too.
        if (__tsan_release) __tsan_release(mutex);
        rc = libpthread_cond_timedwait(cond, mutex, &wait_time);
        if (__tsan_acquire) __tsan_acquire(mutex);
        if (rc == 0) {
          // The thread has successfully entered the waiting state.
          libpthread_mutex_lock(&rec_list_lock);
          thrd_record = find_thread_record_mode(pthread_self());

          //Check if this thread was signaled (CV_SIGNALED state)
          condition_variable_status cv_state = get_thread_cv_state(cond_record->vo.cond_state.waiting_threads, thrd_record->vo.thrd_state.id);

          if (cv_state == CV_SIGNALED) {
            // Remove this thread from the queue
            remove_thread_from_queue(cond_record->vo.cond_state.waiting_threads, thrd_record->vo.thrd_state.id);
            cond_record->vo.cond_state.count--;
          }
          else {
            update_thread_cv_state(cond_record->vo.cond_state.waiting_threads,thrd_record->vo.thrd_state.id,CV_WAITING);
          }
          libpthread_mutex_unlock(&rec_list_lock);
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
        } else if (rc != 0 && rc != ETIMEDOUT) {
          // A "true" error: something went wrong with locking
          // and we pass this on to the end user
          return rc;
        }
      }
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE:{
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_ENQUEUE_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      memcpy_v(mb->cnts + sizeof(cond), &mutex, sizeof(mutex));
      thread_handle_after_dmtcp_restart();
      if (__tsan_release) __tsan_release(mutex);
      libpthread_mutex_unlock(mutex);
      mb->type = COND_WAIT_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      memcpy_v(mb->cnts + sizeof(cond), &mutex, sizeof(mutex));
      // thread_handle_after_dmtcp_restart() (used for the COND_ENQUEUE_TYPE
      // round above) is a one-time-per-thread restart check-in: it captures
      // a getcontext() snapshot and posts the quiescence barrier that the
      // background template thread waits on exactly once per real thread. By
      // the time this second round runs, that barrier has already released
      // and set mode to TARGET_BRANCH_AFTER_RESTART, so a second call falls
      // into thread_handle_after_dmtcp_restart()'s `default: libc_abort()`
      // (its mode_on_entry no longer matches DMTCP_RESTART_INTO_BRANCH/
      // TEMPLATE) -- confirmed live via dmesg showing a SIGABRT here. Use the
      // ordinary wake+wait call instead, matching every other wrapper's
      // second-and-later round with the coordinator.
      thread_wake_scheduler_and_wait();
      libpthread_mutex_lock(mutex);
      if (__tsan_acquire) __tsan_acquire(mutex);
      return 0;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_ENQUEUE_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      memcpy_v(mb->cnts + sizeof(cond), &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      if (__tsan_release) __tsan_release(mutex);
      libpthread_mutex_unlock(mutex);
      mb->type = COND_WAIT_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      memcpy_v(mb->cnts + sizeof(cond), &mutex, sizeof(mutex));
      thread_wake_scheduler_and_wait();
      libpthread_mutex_lock(mutex);
      if (__tsan_acquire) __tsan_acquire(mutex);
      return 0;
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

int mc_pthread_cond_signal(pthread_cond_t *cond) {
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD: {
      return libpthread_cond_signal(cond);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *cond_record = find_object_record_mode(cond);
      if (cond_record == NULL) {
        // A real pthread_cond_init() call has the exact same TSan-
        // interceptor-bypass problem pthread_cond_wait/signal do (never
        // reaches mc_pthread_cond_init() under DMTCP), so cond_record can
        // legitimately not exist yet here -- e.g. a producer that signals
        // before any consumer has ever waited on this cond var. Mirror
        // mc_pthread_cond_wait()'s own lazy-init instead of aborting: a
        // signal with no one having waited yet is a well-defined no-op
        // (matches condition_variable_signal::modify()'s own "signal with
        // no waiters is a valid lost wakeup" semantics).
        pthread_t this_thread = pthread_self();
        rec_list *thrd_record = find_thread_record_mode(this_thread);
        visible_object vo = {
          .type = CONDITION_VARIABLE, .location = cond, .cond_state = { .status = CV_INITIALIZED,
          .interacting_thread = thrd_record->vo.thrd_state.id,
          .associated_mutex = NULL, .count = 0, .waiting_threads = create_thread_queue() }
        };
        cond_record = add_rec_entry_record_mode(&vo);
      }
      // Store pre-signal waiting count (only count CV_WAITING threads)
      int cv_waiting_count = 0;
      thread_queue_node* current = cond_record->vo.cond_state.waiting_threads->front;
      while (current != NULL) {
        if (current->thread_cv_state == CV_WAITING) {
          cv_waiting_count++;
        }
        current = current->next;
      }
      cond_record->vo.cond_state.prev_waiting_count = cv_waiting_count;
      libpthread_mutex_unlock(&rec_list_lock);

      int rc = libpthread_cond_signal(cond);
      if (rc == 0) {
        libpthread_mutex_lock(&rec_list_lock);
        runner_id_t waiting_thread = get_waiting_thread_node(cond_record->vo.cond_state.waiting_threads);
        if (!is_queue_empty(cond_record->vo.cond_state.waiting_threads)) {
           // Find first thread in CV_WAITING state
           if (waiting_thread != RID_INVALID) {
            update_thread_cv_state(cond_record->vo.cond_state.waiting_threads, waiting_thread, CV_SIGNALED);
           }
        }
        // After signaling, check if any thread was marked as signaled
        bool any_thread_signaled = false;
        current = cond_record->vo.cond_state.waiting_threads->front;
        while (current != NULL) {
          if (current->thread_cv_state == CV_SIGNALED) {
            any_thread_signaled = true;
            break;
          }
          current = current->next;
        }

        // If no thread was signaled and there were waiting threads, it's a lost wakeup
        if (!any_thread_signaled && cond_record->vo.cond_state.prev_waiting_count > 0) {
          cond_record->vo.cond_state.lost_wakeups++;
          fprintf(stderr, "WARNING: Lost wakeup detected on condition variable %p\n", cond);
        }

        libpthread_mutex_unlock(&rec_list_lock);
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
      // notify_template_thread();
      // thread_await_scheduler();
      thread_handle_after_dmtcp_restart();
      // Do NOT call the real libpthread_cond_signal() here: see
      // doc/glibc-cond-var-desync.txt. mc_pthread_cond_wait() never blocks on
      // the real cond_t in any post-restart mode (it is fully simulated via
      // the mailbox handshake above), so a real signal here serves no
      // purpose for the model checker -- it is pure risk. Specifically, a
      // clone()-resurrected thread can still be genuinely, kernel-level
      // blocked inside a pre-restart real pthread_cond_timedwait() call; a
      // real signal reaching it here would wake it for real, letting it run
      // further application code without the model checker's scheduling
      // permission and breaking DPOR's single-stepping invariant.
      return 0;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_SIGNAL_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_wake_scheduler_and_wait();
      // See the DMTCP_RESTART_INTO_BRANCH/TEMPLATE case above: no real
      // libpthread_cond_signal() call here either.
      return 0;
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

int mc_pthread_cond_broadcast(pthread_cond_t *cond) {
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD: {
      return libpthread_cond_broadcast(cond);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *cond_record = find_object_record_mode(cond);
      if (cond_record == NULL) {
        // See mc_pthread_cond_signal()'s identical lazy-init: a broadcast
        // with no one having waited yet is a well-defined no-op, not
        // undefined behavior -- cond_record can legitimately not exist yet
        // if pthread_cond_init() hasn't reached the model (same TSan-
        // interceptor-bypass problem wait/signal have).
        pthread_t this_thread = pthread_self();
        rec_list *thrd_record = find_thread_record_mode(this_thread);
        visible_object vo = {
          .type = CONDITION_VARIABLE, .location = cond, .cond_state = { .status = CV_INITIALIZED,
          .interacting_thread = thrd_record->vo.thrd_state.id,
          .associated_mutex = NULL, .count = 0, .waiting_threads = create_thread_queue() }
        };
        cond_record = add_rec_entry_record_mode(&vo);
      }
      libpthread_mutex_unlock(&rec_list_lock);
      int rc = libpthread_cond_broadcast(cond);
      if (rc == 0) {
        libpthread_mutex_lock(&rec_list_lock);
        // Mark all waiting threads as signaled
        thread_queue_node* current = cond_record->vo.cond_state.waiting_threads->front;
        while (current != NULL) {
          // Only mark CV_WAITING threads as signaled (not transitional)
          if (current->thread_cv_state == CV_WAITING) {
            update_thread_cv_state(cond_record->vo.cond_state.waiting_threads,
                                   current->thread, CV_SIGNALED);
          }
          current = current->next;
        }
        libpthread_mutex_unlock(&rec_list_lock);
      }
      return rc;
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_BROADCAST_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_handle_after_dmtcp_restart();
      // Do NOT call the real libpthread_cond_broadcast() here: see
      // mc_pthread_cond_signal()'s identical reasoning above and
      // doc/glibc-cond-var-desync.txt.
      return 0;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_BROADCAST_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_wake_scheduler_and_wait();
      // See the DMTCP_RESTART_INTO_BRANCH/TEMPLATE case above: no real
      // libpthread_cond_broadcast() call here either.
      return 0;
    }
    default: {
      libc_abort();
    }
  }
}

int mc_pthread_cond_destroy(pthread_cond_t *cond) {
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD: {
      return libpthread_cond_destroy(cond);
    }
    case RECORD:
    case PRE_CHECKPOINT: {
      libpthread_mutex_lock(&rec_list_lock);
      rec_list *cond_record = find_object_record_mode(cond);
      if (cond_record == NULL) {
        fprintf(stderr,
                "Undefined behavior: attempting to destroy an uninitialized"
                "condition variable %p",
                cond);
        libc_abort();
      }

      // Check if any thread is waiting on this condition variable
      if (!is_queue_empty(cond_record->vo.cond_state.waiting_threads)) {
        fprintf(stderr,
                "Undefined behavior: attempting to destroy a condition variable"
                " that has threads waiting on it %p",
                cond);
        libc_abort();
      }

      libpthread_mutex_unlock(&rec_list_lock);
      int rc = libpthread_cond_destroy(cond);
      if (rc == 0) {
        libpthread_mutex_lock(&rec_list_lock);
        cond_record->vo.cond_state.status = CV_DESTROYED;
        libpthread_mutex_unlock(&rec_list_lock);
      }
      return rc;
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_DESTROY_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_handle_after_dmtcp_restart();
      // Do NOT call the real libpthread_cond_destroy() here: see
      // mc_pthread_cond_signal()'s identical reasoning above and
      // doc/glibc-cond-var-desync.txt. The model checker already tracks
      // CV_DESTROYED itself; the real cond_t's memory is never touched
      // again regardless.
      return 0;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      mb->type = COND_DESTROY_TYPE;
      memcpy_v(mb->cnts, &cond, sizeof(cond));
      thread_wake_scheduler_and_wait();
      // See the DMTCP_RESTART_INTO_BRANCH/TEMPLATE case above: no real
      // libpthread_cond_destroy() call here either.
      return 0;
    }
    default: {
      libc_abort();
    }
  }
}
