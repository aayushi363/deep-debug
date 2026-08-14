// input_cond_lost_wakeup.c — an interleaving-dependent condition-variable
// deadlock (a classic "lost wakeup"), the cond-var analogue of input_deadlock /
// input_sem_abba.
//
// A waiter blocks on a condition variable WITHOUT re-checking a predicate under
// a loop (the bug). A separate thread signals the cv exactly once.
//   - If the waiter reaches pthread_cond_wait BEFORE the signal → it is woken → clean.
//   - If the signal fires BEFORE the waiter is parked in cond_wait → the wakeup is
//     LOST (a signal with no waiter is a no-op) → the waiter blocks forever → deadlock.
//
// So a DPOR search over the interleavings enumerates several traces, a subset of
// which deadlock (the signal-before-wait orderings). Dynamic init (not the
// PTHREAD_*_INITIALIZER static macros) so libmcmini observes the init ops.
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t m;
static pthread_cond_t cv;

static void *waiter(void *arg) {
  (void)arg;
  pthread_mutex_lock(&m);
  printf("[waiter] cond_wait\n");
  pthread_cond_wait(&cv, &m);  // no predicate loop -> lost-wakeup bug
  printf("[waiter] woken\n");
  pthread_mutex_unlock(&m);
  return NULL;
}

static void *signaler(void *arg) {
  (void)arg;
  pthread_mutex_lock(&m);
  printf("[signaler] cond_signal\n");
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&m);
  return NULL;
}

int main(void) {
  pthread_mutex_init(&m, NULL);
  pthread_cond_init(&cv, NULL);
  pthread_t tw, ts;
  pthread_create(&tw, NULL, waiter, NULL);
  pthread_create(&ts, NULL, signaler, NULL);
  pthread_join(tw, NULL);
  pthread_join(ts, NULL);
  printf("[main] done\n");
  return 0;
}
