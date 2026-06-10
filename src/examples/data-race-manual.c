#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

void __mcmini_read(void *addr, size_t size, uintptr_t site_id);
void __mcmini_write(void *addr, size_t size, uintptr_t site_id);

static int shared_counter;

static void *worker(void *arg) {
  (void)arg;
  __mcmini_read(&shared_counter, sizeof(shared_counter), 1);
  int local = shared_counter;
  local++;
  __mcmini_write(&shared_counter, sizeof(shared_counter), 2);
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
