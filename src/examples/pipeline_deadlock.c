/**
 * pipeline_deadlock.c
 *
 * A complex deadlock scenario designed to stress-test a model checker.
 *
 * ==========================================================================
 * SETUP
 * ==========================================================================
 *
 *  Threads:  1 coordinator (thread 0) + 4 workers (threads 1–4)
 *  Barriers: 2
 *    - barrier_phase1: count = 4  (workers 1–4 synchronize after setup)
 *    - barrier_phase2: count = 4  (workers 1–4 synchronize after compute)
 *  Mutex:    protects a shared `result` variable written by thread 1,
 *            then read by thread 3 to decide whether to skip barrier_phase2.
 *
 * ==========================================================================
 * EXECUTION FLOW
 * ==========================================================================
 *
 *  Coordinator (thread 0):
 *    - Spawns workers, then waits for them via pthread_join.
 *    - Does NOT participate in either barrier.
 *
 *  Workers 1, 2, 4 (normal path):
 *    1. barrier_phase1  ← sync point A
 *    2. Lock mutex, compute result (thread 1 sets result = work_value % 2),
 *       unlock mutex.
 *    3. barrier_phase2  ← sync point B
 *
 *  Worker 3 (conditional path — THE BUG):
 *    1. barrier_phase1  ← sync point A
 *    2. Lock mutex, READ result, unlock mutex.
 *    3. if (result == 1) --> FAST PATH: skip barrier_phase2 and return early.
 *       else             --> SLOW PATH: call barrier_phase2 normally.
 *
 * ==========================================================================
 * WHY THIS IS INTERESTING FOR A MODEL CHECKER
 * ==========================================================================
 *
 *  Thread 1's work_value is 7 (odd), so it always writes result = 1.
 *  Thread 3 always reads result = 1 if it reads AFTER thread 1's write.
 *  Thread 3 reads result = 0 (initial) if it reads BEFORE thread 1's write.
 *
 *  Both threads 1 and 3 hold the mutex when accessing `result`, and both
 *  do so AFTER barrier_phase1. However, the mutex does NOT guarantee
 *  ordering between them — thread 3 might legitimately acquire the lock
 *  first.
 *
 *  This creates TWO classes of interleavings:
 *
 *    [SAFE]     Thread 3 acquires mutex BEFORE thread 1 writes:
 *               → thread 3 reads result = 0 → takes slow path → calls
 *                 barrier_phase2 → all 4 workers reach it → NO deadlock.
 *
 *    [DEADLOCK] Thread 3 acquires mutex AFTER thread 1 writes:
 *               → thread 3 reads result = 1 → takes fast path → skips
 *                 barrier_phase2 → only 3 workers reach it (1, 2, 4)
 *                 → barrier_phase2 never fires → DEADLOCK.
 *
 *  A model checker must explore BOTH orderings of the mutex acquisition
 *  to find the deadlocking trace. This tests:
 *    (a) Barrier semantics under partial participation
 *    (b) Mutex-interleaving with barrier arrival ordering
 *    (c) Data-dependent conditional branching on shared state
 *    (d) The ability to distinguish safe vs. deadlocking traces in the
 *        same program (not all traces deadlock)
 *
 * ==========================================================================
 * COMPILE & RUN
 * ==========================================================================
 *
 *  gcc -o pipeline_deadlock pipeline_deadlock.c -lpthread
 *  ./pipeline_deadlock      # may or may not deadlock depending on scheduling
 *
 *  With McMini:
 *  ./mcmini ./pipeline_deadlock   # should find both safe and deadlock traces
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

/* ── Shared state ──────────────────────────────────────────────── */

#define NUM_WORKERS 4      /* threads 1–4 */
#define BARRIER_COUNT 4    /* both barriers require all 4 workers */

pthread_barrier_t barrier_phase1;
pthread_barrier_t barrier_phase2;

pthread_mutex_t result_mutex;  /* initialized explicitly in main() */
int result = 0;            /* written by thread 1, read by thread 3 */

/* ── Per-thread arguments ──────────────────────────────────────── */
typedef struct {
    int id;
    int work_value;   /* simulated input for this worker */
} worker_arg_t;

/* ── Worker thread function ────────────────────────────────────── */
void *worker(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;
    int id         = w->id;
    int work_value = w->work_value;

    printf("[Worker %d] Started (work_value=%d).\n", id, work_value);

    /* ── Phase 1: all workers synchronize before compute ── */
    printf("[Worker %d] Entering barrier_phase1...\n", id);
    pthread_barrier_wait(&barrier_phase1);
    printf("[Worker %d] Passed barrier_phase1.\n", id);

    /* ── Compute phase: protected by mutex ── */
    if (id == 1) {
        /* Thread 1 writes the shared result */
        pthread_mutex_lock(&result_mutex);
        result = work_value % 2;   /* work_value=7 → result=1 (always) */
        printf("[Worker %d] Wrote result = %d.\n", id, result);
        pthread_mutex_unlock(&result_mutex);

    } else if (id == 3) {
        /*
         * Thread 3 reads result and decides whether to participate in
         * barrier_phase2. This is the buggy conditional.
         *
         * If thread 1 has already written result=1, thread 3 takes the
         * fast path and SKIPS barrier_phase2 → deadlock.
         * If thread 3 reads before thread 1 writes, result=0 → slow path
         * → no deadlock.
         */
        pthread_mutex_lock(&result_mutex);
        int local_result = result;
        printf("[Worker %d] Read result = %d.\n", id, local_result);
        pthread_mutex_unlock(&result_mutex);

        if (local_result == 1) {
            printf("[Worker %d] Fast path: skipping barrier_phase2! "
                   "(This causes deadlock)\n", id);
            return NULL;   /* <── skips barrier_phase2 */
        }
        printf("[Worker %d] Slow path: proceeding to barrier_phase2.\n", id);

    } else {
        /* Workers 2 and 4: do generic work, always reach barrier_phase2 */
        printf("[Worker %d] Finished compute.\n", id);
    }

    /* ── Phase 2: all workers synchronize after compute ── */
    printf("[Worker %d] Entering barrier_phase2...\n", id);
    pthread_barrier_wait(&barrier_phase2);   /* may block forever */
    printf("[Worker %d] Passed barrier_phase2.\n", id);

    return NULL;
}

/* ── Main / coordinator ────────────────────────────────────────── */
int main(void) {
    pthread_t     threads[NUM_WORKERS];
    worker_arg_t  args[NUM_WORKERS];

    /*
     * work_values chosen deliberately:
     *   thread 1: 7 (odd)  → writes result = 1 → triggers thread 3's fast path
     *   others:   even     → no effect on result
     */
    int work_values[NUM_WORKERS] = { 7, 4, 6, 2 };  /* for workers 1,2,3,4 */

    printf("=== Pipeline Deadlock Demo ===\n");
    printf("barrier_phase1 and barrier_phase2 both require %d workers.\n",
           BARRIER_COUNT);
    printf("Worker 3 will skip barrier_phase2 if result==1 (set by worker 1).\n");
    printf("Whether deadlock occurs depends on mutex acquisition order.\n\n");

    pthread_mutex_init(&result_mutex, NULL);
    pthread_barrier_init(&barrier_phase1, NULL, BARRIER_COUNT);
    pthread_barrier_init(&barrier_phase2, NULL, BARRIER_COUNT);

    /* Spawn workers 1–4 (stored at indices 0–3) */
    for (int i = 0; i < NUM_WORKERS; i++) {
        args[i].id         = i + 1;
        args[i].work_value = work_values[i];
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    /* Coordinator waits — will block forever if deadlock occurs */
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_barrier_destroy(&barrier_phase1);
    pthread_barrier_destroy(&barrier_phase2);
    pthread_mutex_destroy(&result_mutex);

    printf("\n=== All workers completed. No deadlock. ===\n");
    return 0;
}