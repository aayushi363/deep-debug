// Regression test AND checkpoint/restart demo for the barrier arrive/pass DPOR
// dependency relations.
//
// A barrier is initialized with count = 2, but THREE worker threads each call
// pthread_barrier_wait() exactly once. The first two arrivals satisfy the
// barrier and pass; the third arrival rolls into the next generation and waits
// for a second arrival that never comes. That third thread is therefore blocked
// forever, so the program is guaranteed to DEADLOCK in every interleaving.
//
// This exercises the ">count threads on one barrier" regime, where a thread's
// arrival generation is interleaving-dependent. It is the case that motivated
// dropping the (unsound) generation-equality refinement from
// barrier_arrive/barrier_pass depends()/coenabled_with(): with the refinement,
// DPOR could declare a same-thread arrive->pass pair independent and prune a
// required backtracking point.
//
// Expected result under mcmini: a DEADLOCK is reported (one worker stuck at the
// barrier, main blocked joining it).
//
// -----------------------------------------------------------------------------
// Checkpoint / restart usage
// -----------------------------------------------------------------------------
// DELAY_SECONDS makes each worker sleep BEFORE it touches the barrier, so the
// program stays alive long enough for a DMTCP checkpoint to be taken mid-run.
// The sleep is real only during RECORD (mc_sleep -> libc_sleep); during model
// checking after restart it is a no-op (mc_sleep returns immediately in the
// TARGET_BRANCH / DMTCP_RESTART_INTO_* modes), so exploration stays fast and
// the sleep does not perturb the interleavings.
//
// Run so the checkpoint interval fires DURING the pre-barrier delay, e.g.:
//
//     mcmini -i 3 ./build/src/examples/barrier_more_threads_than_count
//
// with any interval comfortably less than DELAY_SECONDS. The checkpoint is then
// taken while all workers are still sleeping (before any barrier op); on restart
// mcmini model-checks the barrier from that clean pre-barrier state and reports
// the deadlock.

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define NUM_THREADS 3
#define BARRIER_COUNT 2

// Seconds each worker sleeps before arriving at the barrier. Keep this larger
// than the `-i` checkpoint interval so the checkpoint lands during the delay.
#define DELAY_SECONDS 60

static pthread_barrier_t barrier;

static void *worker(void *arg) {
  long id = (long)arg;
  printf("worker %ld: sleeping %d s before barrier\n", id, DELAY_SECONDS);
  fflush(stdout);
  sleep(DELAY_SECONDS);
  printf("worker %ld: arriving at barrier\n", id);
  fflush(stdout);
  pthread_barrier_wait(&barrier);
  printf("worker %ld: passed barrier\n", id);
  fflush(stdout);
  return NULL;
}

int main(void) {
  pthread_t threads[NUM_THREADS];

  pthread_barrier_init(&barrier, NULL, BARRIER_COUNT);

  for (long i = 0; i < NUM_THREADS; i++) {
    pthread_create(&threads[i], NULL, worker, (void *)i);
  }

  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }

  pthread_barrier_destroy(&barrier);
  printf("all workers joined\n");  // unreachable: the program deadlocks
  return 0;
}
