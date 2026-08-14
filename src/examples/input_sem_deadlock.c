// input_sem_deadlock.c — a minimal semaphore deadlock, the analogue of
// input_deadlock for semaphores. Two worker threads cross-wait on two
// zero-initialized semaphores: T1 waits on A then would post B; T2 waits on B
// then would post A. Both block on their wait forever (neither reaches its
// post), so every interleaving deadlocks. Used to verify the in-SUT scheduler's
// semaphore support + deadlock oracle (enabled set empty while runners parked).
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>

static sem_t A;
static sem_t B;

static void *t1(void *arg) {
  (void)arg;
  printf("[T1] wait A\n");
  sem_wait(&A);
  printf("[T1] post B\n");
  sem_post(&B);
  return NULL;
}

static void *t2(void *arg) {
  (void)arg;
  printf("[T2] wait B\n");
  sem_wait(&B);
  printf("[T2] post A\n");
  sem_post(&A);
  return NULL;
}

int main(void) {
  sem_init(&A, 0, 0);
  sem_init(&B, 0, 0);
  pthread_t th1, th2;
  pthread_create(&th1, NULL, t1, NULL);
  pthread_create(&th2, NULL, t2, NULL);
  pthread_join(th1, NULL);
  pthread_join(th2, NULL);
  printf("[main] done\n");
  return 0;
}
