#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>  // man 2 open
#include <pthread.h>
#include <sched.h>  // For clone()
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#include "dmtcp.h"
#include "mcmini/mcmini.h"
#include "deadlock_detector.h"

#define SIG_MULTITHREADED_FORK (SIGRTMIN+6)

// ThreadSanitizer Fiber API (weak: resolves to the TSAN runtime only for TSAN
// targets, NULL no-op otherwise). A "fiber" is a TSAN ThreadState decoupled
// from the OS thread. `multithreaded_fork` recreates the pre-checkpoint threads
// with clone() + setcontext (so their original %fs/TLS and pthread descriptor
// are preserved and libmcmini does not intercept the creation). But clone()
// means libtsan never registered these threads, so their cur_thread() is torn
// on resume and the first TSAN-intercepted call (e.g. munmap) faults. Mirroring
// DMTCP's own restore fix (threadlist.cpp: "fresh fiber on restart"), each
// resurrected thread switches onto a fresh fiber the moment it resumes, giving
// it a valid ThreadState before any traced call.
extern void *__tsan_create_fiber(unsigned flags) __attribute__((weak));
extern void __tsan_switch_to_fiber(void *fiber, unsigned flags)
    __attribute__((weak));
extern void __tsan_destroy_fiber(void *fiber) __attribute__((weak));

// TSan fork syscall hooks (weak). `_Fork()` bypasses libtsan's fork()
// interceptor, so TSan's BeforeFork/AfterFork pipeline never runs: the child
// inherits TSan's runtime with internal locks still held and a ThreadRegistry
// full of now-dead parent threads. The first instrumented call in the child
// then faults. Bracketing `_Fork()` with these hooks re-runs that pipeline --
// pre acquires TSan's internal locks (parent), post releases them and, in the
// child, scrubs the registry down to the calling thread. This MUST happen
// before any fiber work, since __tsan_create_fiber itself touches the registry.
extern void __sanitizer_syscall_pre_impl_fork(void) __attribute__((weak));
extern void __sanitizer_syscall_post_impl_fork(long res) __attribute__((weak));

// libc's internal clone (NOT intercepted by libtsan, unlike the public
// clone()). libtsan's clone() interceptor treats every call as a fork
// (ForkChildAfter), which corrupts its thread-slot state for the
// CLONE_THREAD clone restart_child_threads_fast() performs below.
//
// DMTCP ALSO strongly exports its own `__clone` (threadwrappers.cpp), which
// unconditionally aborts unless the calling thread is mid-DMTCP's-own
// pthread_create -- confirmed empirically against a real DMTCP restart:
// "ASSERT ... Thread creation with clone syscall is not supported". Calling
// the symbol `__clone` directly hits THAT interceptor, not libc's real
// implementation, exactly like the public clone() hits libtsan's. Bypass
// both via libc_clone() (interception.c), which resolves the real __clone
// once at process startup -- well before any fork/checkpoint -- alongside
// this codebase's other real-libc forwarding functions (libc_abort(),
// libc_fork(), ...), rather than re-resolving it here via a fresh
// post-_Fork() dlopen/dlsym (which would risk deadlocking on a dynamic-
// linker lock left held by a thread that no longer exists in the child).
// The recreated thread's own fiber switch (see
// thread_handle_after_dmtcp_restart()) then gives it a valid TSan
// ThreadState.

// We probably won't need the '#undef', but just in case a .h file defined it:
#undef dmtcp_mcmini_is_loaded
int dmtcp_mcmini_is_loaded(void) { return 1; }

pthread_t ckpt_pthread_descriptor;
volatile atomic_bool libmcmini_has_recorded_checkpoint_thread;
static sem_t template_thread_sem;
static pthread_cond_t template_thread_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t template_thread_mut = PTHREAD_MUTEX_INITIALIZER;

struct threadinfo {
  pid_t origTid; // Used for debugging, and 'origTid == 0'  if not a valid entry.
  ucontext_t context;
  // fs and gs only used in __x86_64__
  unsigned long fs;
  unsigned long gs;
  // tlsAddr only used in __aarch64__ and __riscv
  // In fact, __riscv has the address in a normal register, restored w/ context.
  unsigned long int tlsAddr;
  // glibc:pthread_create and pthread_self use this, but not the clone call:
  pthread_t pthread_descriptor;
} threadInfos[1000];

static volatile atomic_int threadIdx = 0; // threadInfo[threadIdx] is 'struct threadinfo' for next thread.

#ifdef __x86_64__
# include <asm/prctl.h>
# include <sys/prctl.h>

void getTLSPointer(struct threadinfo *localThreadInfo) {
  assert(syscall(SYS_arch_prctl, ARCH_GET_FS, &localThreadInfo->fs) == 0);
  assert(syscall(SYS_arch_prctl, ARCH_GET_GS, &localThreadInfo->gs) == 0);
}

void setTLSPointer(struct threadinfo *localThreadInfo) {
  assert(syscall(SYS_arch_prctl, 2, ARCH_SET_FS, localThreadInfo->fs) != 0);
  assert(syscall(SYS_arch_prctl, 2, ARCH_SET_GS, localThreadInfo->gs) != 0);
}
#elif defined(__aarch64__)
// 1776 valid for glibc-2.17 and beyond
static void getTLSPointer(struct threadinfo *localThreadInfo) {
  unsigned long int addr;
  asm volatile ("mrs   %0, tpidr_el0" : "=r" (addr));
  localThreadInfo->tlsAddr = addr - 1776;  // sizeof(struct pthread) = 1776
}
static void setTLSPointer(struct threadinfo *localThreadInfo) {
  unsigned long int addr = localThreadInfo->tlsAddr + 1776;
  asm volatile ("msr     tpidr_el0, %[gs]" : :[gs] "r" (addr));
}
#elif defined(__riscv)
void getTLSPointer(struct threadinfo *localThreadInfo)
{
  unsigned long int addr;
  asm volatile ("addi %0, tp, 0" : "=r" (addr));
  localThreadInfo->tlsAddr = addr - 1856;  // sizeof(struct pthread)=1856
}
void setTLSPointer(struct threadinfo *localThreadInfo)
{
  unsigned long int addr = localThreadInfo->tlsAddr + 1856;
  asm volatile("addi tp, %[gs], 0" : : [gs] "r" (addr));
}
#else
#error "getTLSPointer()/setTLSPointer() unimplemented on this architecture"
#endif

static inline int pthreadDescriptorTidOffset() {
#ifdef __x86_64__
  // Since glibc-2.11:
  int offset = 720;  // sizeof(tcbhead_t) + sizeof(list_t)
#elif defined(__aarch64__)
  int offset = 208;
#elif defined(__riscv)
  int offset = 208;
#endif
  return offset;
}

static inline pid_t patchThreadDescriptor(pthread_t pthreadSelf) {
  int offset = pthreadDescriptorTidOffset();
  pid_t oldtid = *(pid_t *)((char *)pthreadSelf + offset);
  // Since glibc.2.25, tid, but not pid, is stored in pthread_t.
  // gettid() supported only in glibc-2.30; So, we use syscall().
  *(pid_t *)((char *)pthreadSelf + offset) = syscall(SYS_gettid);
  return oldtid;
}

static void saveThreadStateBeforeFork(struct threadinfo* threadInfo) {
  threadInfo->origTid = syscall(SYS_gettid);

  // For verification only; not needed for functionality
  pid_t oldTid = patchThreadDescriptor(pthread_self());
  if (oldTid != syscall(SYS_gettid)) {
    fprintf(stderr, "PID %d: multithreaded_fork(): patchThreadDescriptor:"
         "bad offset:\n        Run: DMTCP:util/check-pthread-tid-offset.c\n",
         getpid());
    libc_abort();
  }
  threadInfo->pthread_descriptor = pthread_self();
  getTLSPointer(threadInfo);

  // No manual signal-mask save/restore needed: getcontext()/setcontext()
  // already save/restore the blocked-signal set via ucontext_t's uc_sigmask,
  // even when setcontext() resumes on a brand-new clone()'d OS thread (see
  // child_setcontext_fast() below).
}

static int child_setcontext_fast(void *arg) {
  struct threadinfo* threadInfo = arg;
  setTLSPointer(threadInfo);
  patchThreadDescriptor(threadInfo->pthread_descriptor);
  // Does not return: jumps to the getcontext() call site in
  // thread_handle_after_dmtcp_restart(), restoring uc_sigmask (see
  // saveThreadStateBeforeFork() above) along with the rest of the context.
  setcontext(&(threadInfo->context));
  return 0; // not reached
}

// See wrappers.h for the rationale. threadInfos[0..threadIdx) holds one
// entry per real application thread alive at checkpoint time, NOT one per
// fast_multithreaded_fork() call: the atomic_fetch_add() below runs once
// per thread, before this function's own getcontext(); every later branch
// just resumes that same saved context via setcontext(). Verified:
// threadIdx never exceeded 5 across 13,794 fork() calls in one DPOR run.
bool mc_pthread_is_recreated_thread(pthread_t t) {
  int maxThreadIdx = atomic_load(&threadIdx);
  for (int i = 0; i < maxThreadIdx; i++) {
    if (threadInfos[i].origTid != 0 &&
        pthread_equal(threadInfos[i].pthread_descriptor, t)) {
      return true;
    }
  }
  return false;
}

void restart_child_threads_fast(void) {
  int maxThreadIdx = atomic_load(&threadIdx);
  for (int i = 0; i < maxThreadIdx; i++) {
    // int clone(int (*fn)(void *), void *stack, int flags, void *arg, ...
    //           /* pid_t *parent_tid, void *tls, pid_t *child_tid */ );
    int clone_flags = (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM
                       | CLONE_SIGHAND | CLONE_THREAD
                       | CLONE_SETTLS | CLONE_PARENT_SETTID
                       | CLONE_CHILD_CLEARTID
                       | 0);
    // This is a temporary stack, to be replaced after setcontext.
    // FIXME: This stack is a memory leak.  We should free it later.
    void *stack = malloc(0x10000) + 0x10000 - 128; // 64 KB
    int offset = pthreadDescriptorTidOffset();
    pid_t *ctid = (pid_t*)((char*)threadInfos[i].pthread_descriptor + offset);
    pid_t *ptid = ctid;
    // For more insight, read 'man set_tid_address'.
    // R3: use libc's raw __clone (via libc_clone(), see above), bypassing
    // both libtsan's and DMTCP's own interception of the public clone()/
    // __clone symbols.
    libc_clone(child_setcontext_fast,
               stack,
               clone_flags,
               (void *)&threadInfos[i], ptid, (void *)threadInfos[i].fs, ctid);
  }
}

pid_t fast_multithreaded_fork(void) {
  /*********************************************************************
   *NOTE:  We want to skip the normal fork cleanup
   *       of child threads (e.g., reclaim_stack()).
   *       So, we call an internal version of fork().
   *
   *__libc__fork() calls:
   *
   *../sysdeps/unix/sysv/linux/arch-fork.h:26
   *__libc_fork() calls:
   *_Fork() {
   *  pid_t pid = arch_fork (&THREAD_SELF->tid);
   *  ... robust mutex support ...
   *}
   *
   *../sysdeps/unix/sysv/linux/arch-fork.h:34
   *statinc inline pid_t arch_fork(void *ctid) {
   *  echo 'flags = CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD'
   *  ret = INLINE_SYSCALL_CALL (clone, flags, 0, NULL, ctid, 0);
   *}
   *********************************************************************/
  pid_t _Fork();
  // Re-run TSan's fork pipeline around the raw _Fork() (see the syscall-hook
  // declarations above). pre = BeforeFork (parent acquires TSan locks);
  // post = AfterFork (release; child scrubs the ThreadRegistry).
  if (__sanitizer_syscall_pre_impl_fork != NULL) {
    __sanitizer_syscall_pre_impl_fork();
  }
  int childpid = _Fork();
  if (__sanitizer_syscall_post_impl_fork != NULL) {
    __sanitizer_syscall_post_impl_fork(childpid);
  }
  if (childpid == 0) { // child process
    // R4 (remainder): the forking thread keeps its inherited (fork-copied)
    // TSan ThreadState, whose shadow call stack starts at the parent's
    // fork-time depth and can overflow as this thread keeps running. Switch
    // it onto a fresh fiber too, mirroring the recreated-thread fiber switch
    // in thread_handle_after_dmtcp_restart(). Weak symbol: a no-op for
    // non-TSan targets.
    if (__tsan_switch_to_fiber != NULL) {
      __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
    }
    // _Fork() (unlike a real fork()) deliberately skips pthread_atfork()
    // handlers, so mcmini_log()'s own log_mut can be left stranded locked
    // if some other thread held it at the exact instant _Fork() snapshotted
    // memory for this branch -- see doc/log-mutex-fork-desync.txt. This
    // reset MUST run here, before restart_child_threads_fast() recreates
    // any other thread, while this is still the only OS thread alive in
    // the child, so it can never race a concurrent lock/unlock attempt.
    //
    // NOTE: an earlier version of this fix was applied to
    // src/common/multithreaded_fork.c's multithreaded_fork() instead --
    // that function turned out to be dead code (superseded by this one,
    // fast_multithreaded_fork(), per commit 74679cf), so the fix was never
    // actually active on the real --multithreaded-fork path. This is the
    // correct location.
    mcmini_log_reset_after_fork();
    restart_child_threads_fast();
  }
  return childpid;
}

bool is_checkpoint_thread(void) {
  return pthread_equal(pthread_self(), ckpt_pthread_descriptor);
}

void thread_handle_after_dmtcp_restart(void) {
  // IMPORTANT: There's a potential race between
  // notifying the template thread and accessing
  // the new current mode. We care about how
  // we're supposed to react after restarting. The template
  // thread, once signaled by all userspace threads, will
  // change its mode to `TARGET_TEMPLATE_AFTER_RESTART`.
  enum libmcmini_mode mode_on_entry = get_current_mode();
  const pid_t origPid = mcmini_real_pid(getpid());
  const int origThreadIdx = atomic_fetch_add(&threadIdx, 1);
  struct threadinfo* threadInfo = &threadInfos[origThreadIdx];
  memset(threadInfo, 0x0, sizeof(struct threadinfo));

  saveThreadStateBeforeFork(threadInfo);
  int rc = getcontext(&threadInfo->context);
  assert(rc == 0);

  if (mcmini_real_pid(getpid()) == origPid) {
    // Inside the template process
    notify_template_thread();
  }
  else {
    // Returned from `getcontext()` in the forked child: this thread was just
    // resurrected by `restart_child_threads_fast` via clone() + setcontext.
    // Before any TSAN-intercepted call below, switch onto a fresh TSAN
    // fiber, so this OS thread has a valid ThreadState (clone() bypassed
    // libtsan's pthread_create registration, leaving cur_thread() torn).
    // Weak symbol: a no-op for non-TSAN targets. The fiber is intentionally
    // not destroyed -- the branch process is short-lived and exits after
    // one trace.
    if (__tsan_switch_to_fiber != NULL) {
      __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
    }
  }

  switch (mode_on_entry) {
    case DMTCP_RESTART_INTO_TEMPLATE: {
      log_debug("Restarting into template (DMTCP_RESTART_INTO_TEMPLATE)\n");
      // For userspace threads in the template process, the threads
      // must wait _forever_. Since these userspace threads will eventually
      // become active and start listening to the model checker in the
      // multithreaded fork process, we need to be able to control
      // when these threads should begin listening to the model checker.
      // This is accomplished by using the current "mode" of libmcmini:
      // as long as we're not in the `TARGET_BRANCH_AFTER_RESTART` case, we
      // simply ignore any wakeups
      libpthread_mutex_lock(&template_thread_mut);
      while (get_current_mode() != TARGET_BRANCH_AFTER_RESTART) {
        libpthread_cond_wait(&template_thread_cond, &template_thread_mut);
      }
      libpthread_mutex_unlock(&template_thread_mut);
      break;
    }
    case DMTCP_RESTART_INTO_BRANCH: {
      // In the case of calling `dmtcp_restart` fpr each branch, we
      // are expected to immediately talk to the model checker.
      log_debug("Restarting into branch (DMTCP_RESTART_INTO_BRANCH)\n");
      break;
    }
    default: {
      // If we're in any other mode other than restarting at this point,
      // this is a bug. Note the potential race highlighted above that is
      // mitigated with the local variable.
      //
      // However, this solution assumes that the template thread indeed waits
      // until all userspace threads have notified it. Make sure that the logic
      // for the template thread matches this.
      libc_abort();
    }
  }

  // Finally, we can communicate directly with the model checker `mcmini`.
  thread_await_scheduler();
}

void mc_template_thread_loop_forever(void);
static void *template_thread(void *unused) {
  // Phase 1. The template thread is created at record-time
  // _prior_ to checkpoint. It must not intervene until exclusively
  // after a `dmtcp_restart`.
  //
  // This is accomplished by waiting on a semaphore that is only incremented
  // once the `DMTCP_EVENT_RESTART` is sent to `libmcmini.so`.
  libpthread_sem_wait_loop(&template_thread_sem);

  // Phase 2. Wait for all userspace threads to move into a stable state
  //
  // After the `DMTCP_EVENT_RESTART` event is send to `libmcmini.so`, all
  // userspace threads will resume execution. These userspace threads ,
  //
  // TODO: Even in the event in which we don't need to transfer the records
  // during the RECORD phase, we still wait for the userspace threads to go into
  // the "stable" state. This is probably not needed. Indeed, for the
  // `DMTCP_RESTART_INTO_BRANCH` case, waiting after each `dmtcp_restart` call
  // to ensure a stable recorded state is pointless: we're not going to read it
  // anyway! This is an only a potential optimization for later though.

  // Counting via a live /proc/self/task scan is a TOCTOU race: DMTCP
  // recreates checkpointed threads asynchronously via clone(), so a scan
  // that runs before it has finished recreating all of them undercounts,
  // and this barrier then releases before every thread has actually
  // restarted (confirmed empirically: one thread's own restart-completion
  // signal can arrive after this barrier already declared a "consistent
  // state").
  //
  // head_record_mode instead gives an exact, race-free count: it only ever
  // gets THREAD entries for genuine target threads (the template thread and
  // the checkpoint thread never go through libmcmini's wrapped
  // pthread_create(), so neither is ever recorded here), and -- since a
  // DMTCP checkpoint is a full memory snapshot -- this list is preserved
  // exactly as it was at record time across every restart, with no
  // dependence on restart-time scheduling.
  int thread_count = 0;
  for (rec_list *entry = head_record_mode; entry != NULL; entry = entry->next) {
    if (entry->vo.type == THREAD && entry->vo.thrd_state.status == ALIVE) {
      thread_count++;
    }
  }
  log_debug(
      "There are %d threads... waiting for them to get into a consistent "
      "state...\n",
      thread_count);
  for (int i = 0; i < thread_count; i++) {
    libpthread_sem_wait_loop(&dmtcp_restart_sem);
  }
  log_debug("The threads are now in a consistent state\n");

  if (get_current_mode() == DMTCP_RESTART_INTO_BRANCH) {
    atexit(&mc_exit_main_thread_in_child);
    set_current_mode(TARGET_BRANCH_AFTER_RESTART);
  }

  volatile struct mcmini_shm_file *shm_file = global_shm_start;
  volatile struct template_process_t *tpt = &shm_file->tpt;
  pid_t target_branch_pid = mcmini_real_pid(getpid());
  tpt->cpid = target_branch_pid;
  libpthread_sem_post((sem_t *)&tpt->mcmini_process_sem);

  if (get_current_mode() == DMTCP_RESTART_INTO_TEMPLATE) {
    log_debug("Restarting into template (DMTCP_RESTART_INTO_TEMPLATE)\n");
    mc_template_thread_loop_forever();

    // Reaching this point means that we're in the branch: the
    // parent process (aka the template) will never exit
    // the above call to `mc_template_process_loop_forever()`.
  set_current_mode(TARGET_BRANCH_AFTER_RESTART);

  // Phase I has completed for this process; disable the deadlock
  // detector to avoid false positives during later phases.
  mc_install_deadlock_detector(false);

    // Recall that the userspace threads in the template process
    // were idling/doing nothing. Indeed, those threads exist ONLY
    // to ensure that `multithreaded_fork()` clones them.
    //
    // Now that we're finally in the branch, we can signal each
    // of these userspace threads that checkpointing has begun.
    libpthread_mutex_lock(&template_thread_mut);
    libpthread_cond_broadcast(&template_thread_cond);
    libpthread_mutex_unlock(&template_thread_mut);
  }

  // Phase 3. Once in a stable state, check if `mcmini` needs to construct
  // a model of what we've recorded.
  if (getenv("MCMINI_NEEDS_STATE")) {
    log_debug("The template thread is transferring state");

    // FIXME: There appears to be an issue with opening the FIFO
    // here. If it already exists most likely it should be replaced,
    // but we seem to block on both sides (the `McMini` process side and here)
    // or else exit with `No file or directory`. It's probably a race.
    //
    // The current work around is to simply remove the named FIFO manually
    // and run a few exections until the race "resolves" itself (just hope that
    // they don't block).
    int rc = mkfifo("/tmp/mcmini-fifo", S_IRUSR | S_IWUSR);
    if (rc != 0) {
      perror("mkfifo");
    }
    int fd = open("/tmp/mcmini-fifo", O_WRONLY);
    if (fd == -1) {
      perror("open");
      mc_exit(EXIT_FAILURE);
    }
    for (rec_list *entry = head_record_mode; entry != NULL;
         entry = entry->next) {
      if (entry->vo.type == MUTEX) {
        log_verbose("Writing mutex entry %p (state %d, owner %d)\n", entry->vo.location,
               entry->vo.mut_state.status, entry->vo.mut_state.owner);
      } else if (entry->vo.type == THREAD) {
        log_verbose("Writing thread entry %p (id %d, status: %d)\n",
               (void *)entry->vo.thrd_state.pthread_desc,
               entry->vo.thrd_state.id, entry->vo.thrd_state.status);
      } else if (entry->vo.type == CONDITION_VARIABLE) {
        log_verbose("Writing condition variable entry %p (status %d) (count %d) (waiting_queue %p)\n",
               entry->vo.location, entry->vo.cond_state.status, entry->vo.cond_state.waiting_threads->size,
               entry->vo.cond_state.waiting_threads);
      } else if (entry->vo.type == SEMAPHORE) {
        log_verbose("Writing semaphore entry %p (count %d, status: %d)\n",
               (void *)entry->vo.location,
               entry->vo.sem_state.count, entry->vo.sem_state.status);
      } else {
        libc_abort();
      }
      if (entry->vo.type == CONDITION_VARIABLE) {
        // Here we are trying to modify the variables that are a part of union,
        // so doing so is not ideal because it will change other varibale as memory is overlapped.
        thread_queue_node* current = entry->vo.cond_state.waiting_threads->front;
        while(current != NULL){
          visible_object waiting_queue_vo = {
            .type = CV_WAITERS_QUEUE,
            .location = current,
            .waiting_queue_state.cv_location = entry->vo.location,
            .waiting_queue_state.waiting_id = current->thread,
            .waiting_queue_state.cv_state = current->thread_cv_state
          };
          int sz = write(fd, &waiting_queue_vo, sizeof(visible_object));
          assert(sz == sizeof(visible_object));
          current = current->next;
        }
        int sz = write(fd, &entry->vo, sizeof(visible_object));
        assert(sz == sizeof(visible_object));
      }
      else {
        int sz = write(fd, &entry->vo, sizeof(visible_object));
        assert(sz == sizeof(visible_object));
      }
    }
    int sz = write(fd, &empty_visible_obj, sizeof(empty_visible_obj));
    assert(sz == sizeof(visible_object));
    fsync(fd);
    fsync(0);
    close(fd);
  }

  // Exiting from the template thread is fine:
  // once we're in the target branch, we no longer care
  // about it anyways.
  //
  // NOTE: This is true for both repeated `dmtcp_restart` AND for multithreaded
  // forking.
  log_debug("The template thread has completed: exiting...");
  return NULL;
}
// static void SegvfaultHandler(int signum, siginfo_t *siginfo, void *context) {
//   while(1);
// }
// static int AddSegvHandler() {
//   struct sigaction act;
//   static struct sigaction old_act;

//   act.sa_sigaction = &SegvfaultHandler;
//   act.sa_flags = SA_RESTART | SA_SIGINFO;
//   sigemptyset(&act.sa_mask);
//   if (sigaction(SIGSEGV, &act, &old_act)) {
//     perror("Failed to install segv handler");
//     return -1;
//   }
//   if (sigaction(SIGABRT, &act, &old_act)) {
//     perror("Failed to install segv handler");
//     return -1;
//   }
//   return 0;
// }

__attribute__((constructor)) void libmcmini_event_late_init() {
  // AddSegvHandler();
  if (!dmtcp_is_enabled()) {
    return;
  }

  // Initialization should NOT happen in any of the
  // `pthread*` wrapper functions, as they may be called
  // in unexpected ways prior to the `DMTCP_EVENT_INIT`.
  //
  // For example, `libatomic.so` on aarch64 calls `pthread_mutex_lock`
  // as part of its implementation (silly but so it is). This calls
  // `libmcmini.so`'s `pthread_mutex_lock` since McMini comes first.
  // If we called `libmcmini_init()` then, it's possible to end back
  // up in DMTCP (via `dlopen(3)` which DMTCP intercepts) and subsequently
  // call `pthread_mutex_lock` through `libatomic.so` _all by the same thread_.
  // Since we use `pthread_once()`, this causes a deadlock.
  //
  // Hence, we initialization ONLY NOW, and there is not danger that the
  // wrapper functions will unexpectedly recurse on themselves.
  libmcmini_init();

  // We also initialize the semaphore used by the wrapper functions
  // AFTER DMTCP restart. This ensures that the semaphore is properly
  // initialized at restart time.
  libpthread_sem_init(&dmtcp_restart_sem, 0, 0);

  // We would prefer to create the template thread
  // only during restart. However, the restart event
  // is handled by the checkpoint thread. The DMTCP
  // checkpoint thread does not expect to create an
  // extra user thread as a child of the checkpoint
  // thread. So we create the template thread here.
  //
  // The problem with `pthread_create` is that both
  // DMTCP, libpthread.so, and libmcmini.so define it.
  // We don't want to use `libmcmini's` `pthread_create`,
  // because the template thread is a `mcmini` helper thread
  // and not a user thread. We also don't want to use
  // `libpthread.so` directly, or else DMTCP won't
  // know about the thread. So we're left with DMTCP's
  // `pthread_create`.
  //
  // However, we _cannot_ create the template thread
  // upon receiving the `DMTCP_EVENT_INIT` because we need
  // to ensure that any DMTCP resources used by DMTCP's own
  // internal plugins are initialized before using the
  // DMTCP resources. DMTCP's `pthread_create` essentially
  // assumes that the DMTCP resources are already initialized.
  pthread_t template_thread_id;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  libpthread_sem_init(&template_thread_sem, 0, 0);
  // Create via the PUBLIC pthread_create (not libdmtcp_pthread_create), so
  // that, when the target is instrumented, libtsan's pthread_create
  // interceptor sees and registers this thread -- otherwise its (absent)
  // TSAN ThreadState makes restart crash inside libtsan's setjmp/longjmp
  // restore. mc_pthread_create recognizes the flag and still routes the
  // actual creation through DMTCP without user-thread machinery. See
  // TSAN-McMini-DMTCP.txt.
  mc_creating_internal_thread = 1;
  pthread_create(&template_thread_id, &attr, &template_thread, NULL);
  mc_creating_internal_thread = 0;
  pthread_attr_destroy(&attr);
}

static void presuspend_eventHook(DmtcpEvent_t event, DmtcpEventData_t *data) {
  switch (event) {
    case DMTCP_EVENT_INIT: {
      // By default, `libmcmini_mode` is set to `PRE_DMTCP_INIT`
      // to indicate that DMTCP has not yet sent the
      // DMTCP_EVENT_INIT to `libmcmini.so`. This ensures that
      // wrapper functions simply forward their calls to the
      // next appropriate function in line. In most cases, this means
      // calling the equivalent function in `libphread.so`.
      //
      // The checkpoint thread will be created immediately after
      // DMTCP sends the `DMTCP_EVENT_INIT` but it has _not_ yet been
      // created. This means that is it not safe to move into `RECORD` mode
      // yet: we have to wait until the checkpoint thread has been created
      // before moving into `RECORD` mode.
      //
      // Why? If we set
      set_current_mode(PRE_CHECKPOINT_THREAD);
      log_verbose("DMTCP_EVENT_INIT");
      break;
    }
    case DMTCP_EVENT_PRESUSPEND:
      log_verbose("DMTCP_EVENT_PRESUSPEND");
      break;
    case DMTCP_EVENT_PRECHECKPOINT: {
      set_current_mode(PRE_CHECKPOINT);
      log_verbose("DMTCP_EVENT_PRECHECKPOINT");
      // Enable the deadlock detector during the pre-checkpoint/record phase
      // so the process will be aborted if it enters deadlock while recording.
      mc_install_deadlock_detector(true);
      break;
    }
    case DMTCP_EVENT_RESUME:
      log_verbose("DMTCP_EVENT_RESUME");
      break;
    case DMTCP_EVENT_RESTART: {
      log_verbose("DMTCP_EVENT_RESTART callback");
      if (getenv("MCMINI_TEMPLATE_LOOP")) {
        set_current_mode(DMTCP_RESTART_INTO_TEMPLATE);
        log_debug("`MCMINI_TEMPLATE_LOOP` was set at restart-time\n");
      } else {
        set_current_mode(DMTCP_RESTART_INTO_BRANCH);
        log_debug("`MCMINI_TEMPLATE_LOOP` was not set at restart-time\n");

        // A DMTCP_RESTART_INTO_BRANCH process explores exactly one trace,
        // then this process is discarded -- it will never legitimately be
        // asked to checkpoint again. Left alone, the checkpoint thread would
        // resume its normal sleep-checkpoint-resume loop and block forever
        // waiting for a checkpoint request from this restart's (one-shot,
        // otherwise idle) coordinator.
        dmtcp_skip_post_restart_checkpoint_loop();
      }

      // During record mode, the shared memory
      // used by the `mcmini` process to control
      // the userspace threads in this process
      // is not allocated. At restart time,
      // the userspace threads may be in the middle
      // of executing wrapper functions. Once they
      // notice that the `DMTCP_EVENT_RESTART` event has
      // been sent, they will want to access this
      // region. Hence, we need to allocate it prior
      // to returning from the checkpoint thread
      //
      // NOTE: `mcmini` ensures that the shared memory region
      // is properly _initialized_, just as with classic
      // model checking.
      char shm_name[100];
      snprintf(shm_name, sizeof(shm_name), "/mcmini-%s-%lu", getenv("USER"),
               (long)mcmini_real_pid(getppid()));
      shm_name[sizeof(shm_name) - 1] = '\0';
      mc_allocate_shared_memory_region(shm_name);

      // A target program may install a signal handler for signals such
      // as SIGSEGV. A common paradigm with such a signal handler is
      // to block forever to allow a user to attach a debugger to
      // the process for debugging. However, McMini relies on receiving
      // the SIGCHLD signal from processes which contain a segfault.
      // Therefore, on restart, we disable any handlers for SIGSEGV
      struct sigaction action;
      action.sa_handler = SIG_DFL;
      sigemptyset(&action.sa_mask);
      sigaction(SIGSEGV, &action, NULL);

      // Moreover, to ensure that the template thread is solely
      // responsible for handling SIGCHLD, by default we block
      // SIGCHLD. Blocking only in the checkpoint thread and unblocking
      // in the template thread works because the man page for
      //`pthread_sigmask(3)` reads:
      //
      // """
      // A new thread inherits a copy of its creator's signal mask.
      // """
      sigset_t sigchld;
      sigemptyset(&sigchld);
      sigaddset(&sigchld, SIGCHLD);
      pthread_sigmask(SIG_BLOCK, &sigchld, NULL);

      // NOTE: The template thread has been sleeping
      // on `template_thread_sem` during the entire
      // record phase. Only at restart time do we
      // actually wake it up to take control of the
      // other userspace threads to prepare for model checking.
      libpthread_sem_post(&template_thread_sem);
      break;
    }
    default:
      break;
  }
}

DmtcpPluginDescriptor_t presuspend_plugin = {
    DMTCP_PLUGIN_API_VERSION, DMTCP_PACKAGE_VERSION,
    "McMini DMTCP Plugin",    "McMini",
    "dmtcp@ccs.neu.edu",      "McMini deep debugging plugin",
    presuspend_eventHook};

DMTCP_DECL_PLUGIN(presuspend_plugin);
