#pragma once

#define _GNU_SOURCE

#include "mcmini/defines.h"
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>

void libmcmini_init(void);
void mc_load_intercepted_pthread_functions(void);

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *mutexattr);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mut);
int pthread_cond_signal(pthread_cond_t *cond);

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                  void *(*routine)(void *), void *arg);
int libpthread_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                  void *(*routine)(void *), void *arg);
int libdmtcp_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                            void *(*routine)(void *), void *arg);
// Resolved via dlsym(RTLD_NEXT, "pthread_create"), NOT libpthread_pthread_create's
// dlopen("libpthread")+dlsym handle: RTLD_NEXT finds whatever comes after
// libmcmini.so in this process's specific load order, which is TSan's own
// pthread_create interceptor in classic (non-DMTCP) mode -- where libmcmini
// loads ahead of libtsan -- letting a genuinely new thread register with TSan
// the normal way. Under DMTCP, where libtsan loads ahead of libmcmini, this
// still resolves past it to real glibc, identical to libpthread_pthread_create
// -- see mc_pthread_create()'s TARGET_BRANCH/TARGET_BRANCH_AFTER_RESTART case.
int tsan_or_real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                void *(*routine)(void *), void *arg);

int pthread_join(pthread_t thread, void**);
int libpthread_pthread_join(pthread_t thread, void**);
int libdmtcp_pthread_join(pthread_t thread, void**);
// TSan-safe (libtsan-bypassing) handle for pthread_timedjoin_np, used by
// mc_pthread_join's RECORD loop. Calling pthread_timedjoin_np directly resolves
// to libtsan's interceptor, which trips a thread-registry CHECK. See
// TSAN-McMini-DMTCP.txt.
int libpthread_timedjoin_np(pthread_t thread, void**, const struct timespec*);

MCMINI_NO_RETURN void pthread_exit(void *);
// TSan-safe (libtsan-bypassing) handle for pthread_exit, used by
// mc_pthread_exit's pre-restart forwarding path.
MCMINI_NO_RETURN void libpthread_pthread_exit(void *);

int libpthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int libpthread_mutex_lock(pthread_mutex_t *);
int libpthread_mutex_trylock(pthread_mutex_t *);
int libpthread_mutex_timedlock(pthread_mutex_t *, struct timespec *);
int libpthread_mutex_unlock(pthread_mutex_t *);
int libpthread_mutex_destroy(pthread_mutex_t *);

int libpthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
int libpthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
int libpthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
int libpthread_cond_signal(pthread_cond_t*);
int libpthread_cond_broadcast(pthread_cond_t*);
int libpthread_cond_destroy(pthread_cond_t*);

int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attr,
                         unsigned count);
int pthread_barrier_wait(pthread_barrier_t *barrier);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int libpthread_barrier_init(pthread_barrier_t *barrier,
                            const pthread_barrierattr_t *attr,
                             unsigned count);
int libpthread_barrier_wait(pthread_barrier_t *barrier);
int libpthread_barrier_destroy(pthread_barrier_t *barrier);

int sem_init(sem_t*, int, unsigned);
int sem_post(sem_t*);
int sem_wait(sem_t*);
int sem_destroy(sem_t *);
int libpthread_sem_init(sem_t*, int, int);
int libpthread_sem_post(sem_t*);
int libpthread_sem_wait(sem_t*);
int libpthread_sem_wait_loop(sem_t*);
int libpthread_sem_destroy(sem_t *);
int libpthread_sem_timedwait(sem_t*, struct timespec *);

unsigned sleep(unsigned);
unsigned libc_sleep(unsigned);

void exit(int);
MCMINI_NO_RETURN void libc_exit(int);

void abort(void);
MCMINI_NO_RETURN void libc_abort(void);

pid_t libc_fork(void);

// Real, uninterposed __clone. Both libtsan (via its public clone()
// interceptor) and DMTCP (via its own strongly-exported __clone in
// threadwrappers.cpp, which aborts unless called from DMTCP's own
// pthread_create) intercept thread creation; restart_child_threads_fast()
// (dmtcp-callback.c) needs the raw libc primitive to bypass both. See
// TSAN-McMini-DMTCP.txt.
int libc_clone(int (*fn)(void *), void *child_stack, int flags, void *arg,
               void *ptid, void *newtls, void *ctid);
