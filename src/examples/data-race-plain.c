#include <pthread.h>

static int shared_counter;

static void *worker(void *arg) {
  (void)arg;
  int local = shared_counter;
  local++;
  shared_counter = local;
  return NULL;
}

int main(void) {
  pthread_t t1;
  pthread_t t2;
  pthread_create(&t1, NULL, worker, NULL);
  pthread_create(&t2, NULL, worker, NULL);
  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  return shared_counter == 2 ? 0 : 1;
}
