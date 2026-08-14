// input_sem_abba.c — a semaphore AB-BA, the direct analogue of input_deadlock
// but using binary semaphores (init count 1) as locks. Two worker threads
// acquire two semaphores in OPPOSITE orders:
//
//   T1: wait A, wait B, post B, post A
//   T2: wait B, wait A, post A, post B
//
// This is interleaving-DEPENDENT: some schedules run clean (one thread finishes
// before the other starts), while the cross schedule deadlocks (T1 holds A and
// waits on B while T2 holds B and waits on A). So a DPOR search over the thread
// interleavings enumerates several distinct traces, a subset of which deadlock —
// exactly the semaphore counterpart of the mutex AB-BA in input_deadlock.
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>

static sem_t A;
static sem_t B;

static void *t1(void *arg) {
  (void)arg;
  printf("[T1] wait A\n");
  sem_wait(&A);
  printf("[T1] wait B\n");
  sem_wait(&B);
  printf("[T1] post B\n");
  sem_post(&B);
  printf("[T1] post A\n");
  sem_post(&A);
  return NULL;
}

static void *t2(void *arg) {
  (void)arg;
  printf("[T2] wait B\n");
  sem_wait(&B);
  printf("[T2] wait A\n");
  sem_wait(&A);
  printf("[T2] post A\n");
  sem_post(&A);
  printf("[T2] post B\n");
  sem_post(&B);
  return NULL;
}

int main(void) {
  sem_init(&A, 0, 1);  // binary semaphores used as locks
  sem_init(&B, 0, 1);
  pthread_t th1, th2;
  pthread_create(&th1, NULL, t1, NULL);
  pthread_create(&th2, NULL, t2, NULL);
  pthread_join(th1, NULL);
  pthread_join(th2, NULL);
  printf("[main] done\n");
  return 0;
}
