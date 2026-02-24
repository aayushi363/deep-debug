#define _GNU_SOURCE
#include "mcmini/spy/intercept/interception.h"

#include <assert.h>
#include <dlfcn.h>
#include <dmtcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

#include "mcmini/spy/intercept/wrappers.h"

pthread_once_t libmcini_init = PTHREAD_ONCE_INIT;

// Set once mc_load_intercepted_pthread_functions() has fully completed.
// Plain flag, not itself pthread_once/TSAN-intercepted. libmcmini_init()
// checks this directly (see below), so that once initialization has truly
// completed, EVERY later call -- from any thread, including ones with no
// TSAN ThreadState of their own -- takes a plain-memory fast path and never
// touches pthread_once()/TSAN's interceptor at all.
static bool libmcmini_init_done = false;

typeof(&pthread_create) libpthread_pthread_create_ptr;
typeof(&pthread_create) libdmtcp_pthread_create_ptr;
typeof(&pthread_create) tsan_or_real_pthread_create_ptr;
typeof(&pthread_join) libpthread_pthread_join_ptr;
typeof(&pthread_join) libdmtcp_pthread_join_ptr;
typeof(&pthread_timedjoin_np) libpthread_timedjoin_np_ptr;
__attribute__((__noreturn__)) typeof(&pthread_exit) libpthread_pthread_exit_ptr;
typeof(&pthread_mutex_init) pthread_mutex_init_ptr;
typeof(&pthread_mutex_lock) pthread_mutex_lock_ptr;
typeof(&pthread_mutex_trylock) pthread_mutex_trylock_ptr;
typeof(&pthread_mutex_timedlock) pthread_mutex_timedlock_ptr;
typeof(&pthread_mutex_unlock) pthread_mutex_unlock_ptr;
typeof(&pthread_mutex_destroy) pthread_mutex_destroy_ptr;
typeof(&sem_wait) sem_wait_ptr;
typeof(&sem_timedwait) sem_timedwait_ptr;
typeof(&sem_post) sem_post_ptr;
typeof(&sem_init) sem_init_ptr;
typeof(&sem_destroy) sem_destroy_ptr;
typeof(&pthread_cond_init) pthread_cond_init_ptr;
typeof(&pthread_cond_wait) pthread_cond_wait_ptr;
typeof(&pthread_cond_timedwait) pthread_cond_timedwait_ptr;
typeof(&pthread_cond_signal) pthread_cond_signal_ptr;
typeof(&pthread_cond_broadcast) pthread_cond_broadcast_ptr;
typeof(&pthread_cond_destroy) pthread_cond_destroy_ptr;
typeof(&pthread_barrier_init) pthread_barrier_init_ptr;
typeof(&pthread_barrier_wait) pthread_barrier_wait_ptr;
typeof(&pthread_barrier_destroy) pthread_barrier_destroy_ptr;
typeof(&sleep) sleep_ptr;
__attribute__((__noreturn__)) typeof(&exit) exit_ptr;
__attribute__((__noreturn__)) typeof(&abort) abort_ptr;
typeof(&fork) fork_ptr;
typeof(&libc_clone) clone_ptr;

void libmcmini_init(void) {
  // Fast path: a brand-new thread that hasn't gone through TSAN's own
  // pthread_create interceptor yet (e.g. TSAN's own lazily-spawned
  // background thread, or a DMTCP-under-TSAN worker thread whose prologue
  // runs before TSAN's registration trampoline -- see wrappers.c's
  // dmtcp_create_checkpoint_thread_wrapper() and mc_thread_routine_wrapper())
  // has no TSAN ThreadState. pthread_once() itself is TSAN-intercepted and
  // crashes dereferencing that nonexistent ThreadState. Once initialization
  // has genuinely completed once (by anyone), there's nothing left to do, so
  // skip pthread_once() -- and the TSAN interceptor call it would make --
  // entirely.
  if (libmcmini_init_done) {
    return;
  }
  pthread_once(&libmcini_init, &mc_load_intercepted_pthread_functions);
  libmcmini_init_done = true;
}

void mc_load_intercepted_pthread_functions(void) {
  void *libpthread_handle =
      dlopen("libpthread.so", RTLD_LAZY);

  if (!libpthread_handle) {
    libpthread_handle = dlopen("libpthread.so.0", RTLD_LAZY);
  }

  if (!libpthread_handle) {
    fprintf(stderr, "dlopen(3) couldn't find `libpthread`: %s\n", dlerror());
    fflush(stderr);
    libc_abort();
  }

  void *libc_handle = dlopen("libc.so", RTLD_LAZY);
  if (!libc_handle) {
    libc_handle = dlopen("libc.so.6", RTLD_LAZY);
  }

  if (!libc_handle) {
    fprintf(stderr, "dlopen(3) couldn't find `libc`: %s\n", dlerror());
    fflush(stderr);
    libc_abort();
  }

  libpthread_pthread_create_ptr = dlsym(libpthread_handle, "pthread_create");
  // See tsan_or_real_pthread_create()'s header comment for why this must be
  // RTLD_NEXT, not the libpthread_handle dlsym above.
  tsan_or_real_pthread_create_ptr = dlsym(RTLD_NEXT, "pthread_create");
  libpthread_pthread_join_ptr = dlsym(libpthread_handle, "pthread_join");
  libpthread_timedjoin_np_ptr = dlsym(libpthread_handle, "pthread_timedjoin_np");
  // In classic model checking (no DMTCP), resolve `pthread_create` via
  // RTLD_NEXT rather than the explicit `libpthread` handle. Because
  // `libmcmini.so` is LD_PRELOAD'd ahead of the target's DT_NEEDED libraries,
  // RTLD_NEXT lands on the *next* `pthread_create` in the link map -- a
  // sanitizer's interceptor (e.g. libtsan's) when the target is instrumented,
  // and libc/libpthread otherwise. Going through the interceptor lets a
  // sanitizer initialize its per-thread state for threads that libmcmini spawns
  // in TARGET_BRANCH mode (see wrappers.c:mc_pthread_create); calling libc's
  // `pthread_create` directly (the explicit handle) left that state
  // uninitialized and crashed on the first instrumented access in the child.
  //
  // Under DMTCP we keep the explicit `libpthread` handle unchanged: libdmtcp is
  // also in the link map, so RTLD_NEXT could resolve to DMTCP's interceptor,
  // and the DMTCP thread-creation paths are handled separately via
  // `libdmtcp_pthread_create`. (Sanitizer support under deep debugging is
  // future work.)
  if (dmtcp_is_enabled()) {
    libpthread_pthread_create_ptr = dlsym(libpthread_handle, "pthread_create");
  } else {
    libpthread_pthread_create_ptr = dlsym(RTLD_NEXT, "pthread_create");
    if (!libpthread_pthread_create_ptr)
      libpthread_pthread_create_ptr =
          dlsym(libpthread_handle, "pthread_create");
  }
  libpthread_pthread_exit_ptr = dlsym(libpthread_handle, "pthread_exit");
  pthread_mutex_init_ptr = dlsym(libpthread_handle, "pthread_mutex_init");
  pthread_mutex_lock_ptr = dlsym(libpthread_handle, "pthread_mutex_lock");
  pthread_mutex_trylock_ptr = dlsym(libpthread_handle, "pthread_mutex_trylock");
  pthread_mutex_timedlock_ptr = dlsym(libpthread_handle, "pthread_mutex_timedlock");
  pthread_mutex_unlock_ptr = dlsym(libpthread_handle, "pthread_mutex_unlock");
  pthread_mutex_destroy_ptr = dlsym(libpthread_handle, "pthread_mutex_destroy");
  sem_timedwait_ptr = dlsym(libpthread_handle, "sem_timedwait");
  sem_wait_ptr = dlsym(libpthread_handle, "sem_wait");
  sem_timedwait_ptr = dlsym(libpthread_handle, "sem_timedwait");
  sem_post_ptr = dlsym(libpthread_handle, "sem_post");
  sem_init_ptr = dlsym(libpthread_handle, "sem_init");
  sem_destroy_ptr = dlsym(libpthread_handle, "sem_destroy");
  pthread_cond_init_ptr = dlsym(libpthread_handle, "pthread_cond_init");
  pthread_cond_wait_ptr = dlsym(libpthread_handle, "pthread_cond_wait");
  pthread_cond_timedwait_ptr = dlsym(libpthread_handle, "pthread_cond_timedwait");
  pthread_cond_signal_ptr = dlsym(libpthread_handle, "pthread_cond_signal");
  pthread_cond_broadcast_ptr = dlsym(libpthread_handle, "pthread_cond_broadcast");
  pthread_cond_destroy_ptr = dlsym(libpthread_handle, "pthread_cond_destroy");
  pthread_barrier_init_ptr = dlsym(libpthread_handle, "pthread_barrier_init");
  pthread_barrier_wait_ptr = dlsym(libpthread_handle, "pthread_barrier_wait");
  pthread_barrier_destroy_ptr = dlsym(libpthread_handle, "pthread_barrier_destroy");
  sleep_ptr = dlsym(libc_handle, "sleep");
  exit_ptr = dlsym(libc_handle, "exit");
  abort_ptr = dlsym(libc_handle, "abort");
  fork_ptr = dlsym(libc_handle, "fork");
  clone_ptr = dlsym(libc_handle, "__clone");
  dlclose(libpthread_handle);
  dlclose(libc_handle);

  if (dmtcp_is_enabled()) {
    // TODO: This shouldn't refer to the absolute path to dmtcp.so
    // We need to -->
    void *libdmtcp_handle = dlopen("libdmtcp.so", RTLD_LAZY);
    if (!libdmtcp_handle) {
      fprintf(stderr, "dlopen(3) couldn't find `libdmtcp`: %s\n", dlerror());
      fflush(stderr);
      libc_abort();
    }
    libdmtcp_pthread_create_ptr = dlsym(libdmtcp_handle, "pthread_create");
    libdmtcp_pthread_join_ptr = dlsym(libdmtcp_handle, "pthread_join");
    dlclose(libdmtcp_handle);
  }
}

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *mutexattr) {
  return mc_pthread_mutex_init(mutex, mutexattr);
}

int libpthread_mutex_init(pthread_mutex_t *mutex,
                          const pthread_mutexattr_t *attr) {
  libmcmini_init();
  return (*pthread_mutex_init_ptr)(mutex, attr);
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
  return mc_pthread_mutex_lock(mutex);
}

int libpthread_mutex_lock(pthread_mutex_t *mut) {
  libmcmini_init();
  return (*pthread_mutex_lock_ptr)(mut);
}

int libpthread_mutex_trylock(pthread_mutex_t *mut) {
  libmcmini_init();
  return (*pthread_mutex_trylock_ptr)(mut);
}

int libpthread_mutex_timedlock(pthread_mutex_t *mut, struct timespec *t) {
  libmcmini_init();
  return (*pthread_mutex_timedlock_ptr)(mut, t);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  return mc_pthread_mutex_unlock(mutex);
}

int libpthread_mutex_unlock(pthread_mutex_t *mut) {
  libmcmini_init();
  return (*pthread_mutex_unlock_ptr)(mut);
}

int libpthread_mutex_destroy(pthread_mutex_t *mut) {
  libmcmini_init();
  return (*pthread_mutex_destroy_ptr)(mut);
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
  return mc_pthread_cond_init(cond, attr);
}

int libpthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
  libmcmini_init();
  return (*pthread_cond_init_ptr)(cond, attr);
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mut) {
  return mc_pthread_cond_wait(cond, mut);
}

int libpthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mut) {
  libmcmini_init();
  return (*pthread_cond_wait_ptr)(cond, mut);
}

int libpthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mut,
                              const struct timespec *abstime) {
  libmcmini_init();
  return (*pthread_cond_timedwait_ptr)(cond, mut, abstime);
}

int pthread_cond_signal(pthread_cond_t *cond) {
  return mc_pthread_cond_signal(cond);
}

int libpthread_cond_signal(pthread_cond_t *cond) {
  libmcmini_init();
  return (*pthread_cond_signal_ptr)(cond);
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
  return mc_pthread_cond_broadcast(cond);
}

int libpthread_cond_broadcast(pthread_cond_t *cond) {
  libmcmini_init();
  return (*pthread_cond_broadcast_ptr)(cond);
}

int pthread_cond_destroy(pthread_cond_t *cond) {
  return mc_pthread_cond_destroy(cond);
}

int libpthread_cond_destroy(pthread_cond_t *cond) {
  libmcmini_init();
  return (*pthread_cond_destroy_ptr)(cond);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*routine)(void *), void *arg) {
  return mc_pthread_create(thread, attr, routine, arg);
}

int libpthread_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                              void *(*routine)(void *), void *arg) {
  libmcmini_init();
  return (*libpthread_pthread_create_ptr)(thread, attr, routine, arg);
}

int libdmtcp_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                            void *(*routine)(void *), void *arg) {
  libmcmini_init();
  return (*libdmtcp_pthread_create_ptr)(thread, attr, routine, arg);
}

int tsan_or_real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                void *(*routine)(void *), void *arg) {
  libmcmini_init();
  return (*tsan_or_real_pthread_create_ptr)(thread, attr, routine, arg);
}

int pthread_join(pthread_t thread, void **rv) {
  return mc_pthread_join(thread, rv);
}
int libpthread_pthread_join(pthread_t thread, void **rv) {
  libmcmini_init();
  return (*libpthread_pthread_join_ptr)(thread, rv);
}
int libpthread_timedjoin_np(pthread_t thread, void **rv,
                            const struct timespec *abstime) {
  libmcmini_init();
  return (*libpthread_timedjoin_np_ptr)(thread, rv, abstime);
}
int libdmtcp_pthread_join(pthread_t thread, void **rv) {
  libmcmini_init();
  return (*libdmtcp_pthread_join_ptr)(thread, rv);
}

MCMINI_NO_RETURN void pthread_exit(void *retval) {
  mc_pthread_exit(retval);
}
MCMINI_NO_RETURN void libpthread_pthread_exit(void *retval) {
  libmcmini_init();
  (*libpthread_pthread_exit_ptr)(retval);
}

void exit(int status) {
  mc_transparent_exit(status);
}

void abort(void) {
  mc_transparent_abort();
}

unsigned sleep(unsigned duration) {
  return mc_sleep(duration);
}

unsigned libc_sleep(unsigned duration) {
  libmcmini_init();
  return (*sleep_ptr)(duration);
}

// Forwarding methods to the underlying libraries
MCMINI_NO_RETURN void libc_abort(void) {
  libmcmini_init();
  (*abort_ptr)();
}
MCMINI_NO_RETURN void libc_exit(int status) {
  libmcmini_init();
  (*exit_ptr)(status);
}
pid_t libc_fork(void) {
  libmcmini_init();
  return (*fork_ptr)();
}
int libc_clone(int (*fn)(void *), void *child_stack, int flags, void *arg,
               void *ptid, void *newtls, void *ctid) {
  libmcmini_init();
  return (*clone_ptr)(fn, child_stack, flags, arg, ptid, newtls, ctid);
}

int sem_init(sem_t*sem, int p, unsigned count) {
  return mc_sem_init(sem, p, count);
}
int sem_destroy(sem_t *sem) { return mc_sem_destroy(sem); }
int libpthread_sem_init(sem_t *sem, int pshared, int value) {
  libmcmini_init();
  return (*sem_init_ptr)(sem, pshared, value);
}
int libpthread_sem_destroy(sem_t *sem) {
  libmcmini_init();
  return (*sem_destroy_ptr)(sem);
}
int sem_post(sem_t* sem) {
  return mc_sem_post(sem);
}
int libpthread_sem_post(sem_t *sem) {
  libmcmini_init();
  return (*sem_post_ptr)(sem);
}
int sem_wait(sem_t *sem) {
  return mc_sem_wait(sem);
}
int libpthread_sem_wait(sem_t *sem) {
  libmcmini_init();
  return (*sem_wait_ptr)(sem);
}
int libpthread_sem_timedwait(sem_t *sem, struct timespec *ts) {
  libmcmini_init();
  return (*sem_timedwait_ptr)(sem, ts);
}
int libpthread_sem_wait_loop(sem_t *sem) {
   // retry on interruption
  int rc = libpthread_sem_wait(sem);
  while (rc == -1 && errno == EINTR)
    rc = libpthread_sem_wait(sem);
  return rc;
}

// __libc_start_main() is called from _start (crt1.o), not glibc-internal
// code, so unlike exit()/_exit() -- whose implicit post-main() call chain
// resolves through glibc-internal hidden aliases that bypass any public-
// symbol interposition (see test/tsan_support/test_implicit_exit_interposition.c)
// -- this call site goes through ordinary dynamic symbol resolution and can
// be intercepted like any other public symbol. Used to guarantee
// mc_transparent_exit()'s exit-transition/restart-quiescence handling runs
// after the target's main() returns, even when it returns plainly
// (`return N;`) rather than calling exit()/pthread_exit() explicitly --
// verified in test/tsan_support/test_libc_start_main_hook.c.
typedef int (*main_fn)(int, char **, char **);
typedef int (*libc_start_main_fn)(main_fn, int, char **, void (*)(void),
                                  void (*)(void), void (*)(void), void *);

static main_fn real_main;

static int wrapped_main(int argc, char **argv, char **envp) {
  int rc = real_main(argc, argv, envp);
  libmcmini_init();
  mc_transparent_exit(rc);
}

int __libc_start_main(main_fn main, int argc, char **argv, void (*init)(void),
                      void (*fini)(void), void (*rtld_fini)(void),
                      void *stack_end) {
  real_main = main;
  libc_start_main_fn real_start_main =
      (libc_start_main_fn)dlsym(RTLD_NEXT, "__libc_start_main");
  return real_start_main(wrapped_main, argc, argv, init, fini, rtld_fini,
                         stack_end);
}

int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attr,
                         unsigned count) {
  return mc_pthread_barrier_init(barrier, attr, count);
}

int libpthread_barrier_init(pthread_barrier_t *barrier,
                            const pthread_barrierattr_t *attr,
                            unsigned count) {
  libmcmini_init();
  return (*pthread_barrier_init_ptr)(barrier, attr, count);
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
  return mc_pthread_barrier_wait(barrier);
}

int libpthread_barrier_wait(pthread_barrier_t *barrier) {
  libmcmini_init();
  return (*pthread_barrier_wait_ptr)(barrier);
}

int pthread_barrier_destroy(pthread_barrier_t *barrier) {
  return mc_pthread_barrier_destroy(barrier);
}

int libpthread_barrier_destroy(pthread_barrier_t *barrier) {
  libmcmini_init();
  return (*pthread_barrier_destroy_ptr)(barrier);
}
