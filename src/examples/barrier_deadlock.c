/**
 * barrier_deadlock.c
 *
 * KNOWN DEADLOCK DEMONSTRATION using pthread_barrier
 *
 * HOW THE DEADLOCK OCCURS:
 * -------------------------
 * A pthread_barrier requires exactly N threads to call pthread_barrier_wait()
 * before any of them are released. In this program:
 *
 *   - The barrier is initialized with count = 4 (expects 4 threads).
 *   - Only 3 threads are created.
 *   - Thread #1 (id=1) hits an early return BEFORE reaching the barrier.
 *
 * This means only 2 out of 4 required threads ever call barrier_wait().
 * The barrier count is never satisfied → all waiting threads block FOREVER.
 *
 * DEADLOCK SUMMARY:
 *   Barrier initialized with: 4
 *   Threads created:          3
 *   Threads that bail early:  1  (thread id == 1)
 *   Threads that reach wait:  2  ← never enough to release the barrier
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define NUM_THREADS     3   /* threads created */
#define BARRIER_COUNT   4   /* barrier expects this many — intentional mismatch */

pthread_barrier_t barrier;

void *worker(void *arg) {
    int id = *(int *)arg;

    printf("[Thread %d] Started.\n", id);

    /* Simulate some work */
    sleep(1);

    /* BUG: Thread 1 returns early without calling barrier_wait.
     * This means the barrier will never accumulate enough participants.
     * All other threads will wait forever → DEADLOCK.
     */
    if (id == 1) {
        printf("[Thread %d] Exiting early — skipping barrier!\n", id);
        return NULL;  /* <-- causes deadlock for everyone else */
    }

    printf("[Thread %d] Waiting at barrier...\n", id);
    pthread_barrier_wait(&barrier);  /* blocks indefinitely */

    /* This line is never reached by any thread */
    printf("[Thread %d] Passed the barrier!\n", id);
    return NULL;
}

int main(void) {
    pthread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];

    printf("Initializing barrier for %d threads, but only %d will be created.\n",
           BARRIER_COUNT, NUM_THREADS);
    printf("Additionally, one thread will exit early. Deadlock is guaranteed.\n\n");

    pthread_barrier_init(&barrier, NULL, BARRIER_COUNT);

    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);  /* main also blocks forever here */
    }

    /* Never reached */
    pthread_barrier_destroy(&barrier);
    printf("Done.\n");
    return 0;
}