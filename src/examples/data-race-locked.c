/*
 * Negative test for the Phase-1 lockset predictor.
 *
 * Identical to data-race-plain.c, except every access to `shared_counter` is
 * guarded by the same mutex. The lockset predictor should therefore stay
 * SILENT: a common lock is always held when the counter is touched, so the
 * candidate lockset never empties. (Compare with data-race-plain.c, which has
 * no lock and should be flagged.)
 */
#include <pthread.h>

static int shared_counter;
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&counter_lock);
  int local = shared_counter;
  local++;
  shared_counter = local;
  pthread_mutex_unlock(&counter_lock);
  return NULL;
}

int main(void) {
  pthread_t t1;
  pthread_t t2;
  pthread_create(&t1, NULL, worker, NULL);
  pthread_create(&t2, NULL, worker, NULL);
  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  // Read under the lock too. The join already orders this read after both
  // workers, so it is race-free either way -- but the lockset predictor does
  // not reason about happens-before from join(), so an *unlocked* read here
  // would be a (false) positive. Guarding it keeps every access to
  // shared_counter under a common lock, which is what makes this a clean
  // negative test.
  pthread_mutex_lock(&counter_lock);
  int result = shared_counter == 2 ? 0 : 1;
  pthread_mutex_unlock(&counter_lock);
  return result;
}
