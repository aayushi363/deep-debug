#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>

#include "mcmini/mcmini.h"
#include "deadlock_detector.h"

/*
 * Barrier interception DISABLED for testing.
 * All three wrappers pass straight through to the real pthreads library so
 * that McMini creates no barrier model objects.  This lets us determine
 * whether the barrier objects appearing in the model originate from DMTCP
 * internals (they disappear) or from McMini's own condvar pattern-matching
 * (they persist).
 *
 * To re-enable, restore the full switch-statement bodies from git history.
 */

int mc_pthread_barrier_init(pthread_barrier_t *barrier,
                             const pthread_barrierattr_t *attr,
                             unsigned count) {
  return libpthread_barrier_init(barrier, attr, count);
}

int mc_pthread_barrier_destroy(pthread_barrier_t *barrier) {
  return libpthread_barrier_destroy(barrier);
}

int mc_pthread_barrier_wait(pthread_barrier_t *barrier) {
  return libpthread_barrier_wait(barrier);
}
