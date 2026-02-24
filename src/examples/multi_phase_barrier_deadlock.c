/**
 * multi_phase_barrier_deadlock.c
 *
 * Complex pthread_barrier stress test for McMini.
 *
 * ==========================================================================
 * SETUP
 * ==========================================================================
 *
 *  Threads:  1 coordinator (T0/main) + 3 workers (T1, T2, T3)
 *  Barriers:
 *    - round_barrier  (count=3): T1, T2, T3 sync at the end of EACH round.
 *                                Used TWICE → tests barrier reuse (gen:0 and gen:1).
 *    - check_barrier  (count=2): T1 and T2 ONLY sync mid-round-2 to sequence their
 *                                mutex accesses.  Always fires (T3 never touches it).
 *  Mutex:    protects shared `early_exit` flag.
 *
 * ==========================================================================
 * EXECUTION FLOW
 * ==========================================================================
 *
 *  ROUND 1 — always safe, tests gen:0
 *  ─────────────────────────────────
 *  T1, T2, T3: do independent work → round_barrier (gen:0) → all pass.
 *
 *  ROUND 2 — conditional, tests gen:1 and barrier-reuse
 *  ──────────────────────────────────────────────────────
 *  T1: lock → write early_exit=1 → unlock
 *          → check_barrier              (sync with T2 only)
 *          → round_barrier (gen:1)
 *
 *  T2: check_barrier                    (sync with T1 first)
 *          → lock → read early_exit → unlock
 *          → round_barrier (gen:1)
 *
 *  T3: lock → read early_exit → unlock
 *          if early_exit == 1  →  return early  ← SKIPS round_barrier(gen:1)
 *          else                →  round_barrier (gen:1)
 *
 * ==========================================================================
 * DEADLOCK vs SAFE
 * ==========================================================================
 *
 *  [DEADLOCK] T1 acquires the mutex before T3:
 *             T1 writes early_exit=1, then releases.
 *             T3 reads early_exit=1 → skips round_barrier(gen:1).
 *             Only T1 and T2 arrive at round_barrier(gen:1); count=3 → never fires.
 *             T0 (main) blocks on pthread_join for T1 and T2 forever.
 *
 *  [SAFE]     T3 acquires the mutex before T1:
 *             T3 reads early_exit=0 → takes the normal path.
 *             T1, T2, T3 all arrive at round_barrier(gen:1) → fires → no deadlock.
 *
 * ==========================================================================
 * WHAT THIS STRESS-TESTS
 * ==========================================================================
 *
 *  (a) Barrier reuse: round_barrier fires twice (gen:0 then gen:1). Tests that
 *      the generation counter correctly tracks both cycles.
 *  (b) Multiple concurrent barriers: round_barrier and check_barrier interact
 *      in the same round. Check_barrier fires mid-round-2 while round_barrier
 *      is still pending, exercising DPOR with two live barrier objects.
 *  (c) Conditional deadlock: only some mutex orderings cause the deadlock.
 *      McMini must explore both T1-before-T3 and T3-before-T1 to find it.
 *  (d) Mutex × barrier DPOR independence: mutex ops and barrier ops on different
 *      objects must be treated as independent to avoid state-space explosion.
 *  (e) Cross-round data dependency: early_exit is written in round 2 and read
 *      in round 2 by two different threads, racing through the same mutex.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define NUM_WORKERS 3

pthread_barrier_t round_barrier;  /* count=3: all workers sync each round   */
pthread_barrier_t check_barrier;  /* count=2: T1 and T2 sync inside round 2 */
pthread_mutex_t   flag_mutex;

int early_exit = 0;  /* written by T1, read by T3 (and T2 for info) */

typedef struct { int id; } worker_arg;

void *worker_fn(void *arg)
{
    worker_arg *w = (worker_arg *)arg;
    int id = w->id;

    /* ── ROUND 1: all workers participate (tests gen:0) ─────────── */
    printf("[T%d] Round 1: starting work\n", id);

    /* Simulate independent per-worker computation */
    int dummy = id * 42;
    (void)dummy;

    printf("[T%d] Round 1: arriving at round_barrier\n", id);
    pthread_barrier_wait(&round_barrier);
    printf("[T%d] Round 1: passed round_barrier (gen:0)\n", id);

    /* ── ROUND 2: conditional paths (tests gen:1 + barrier reuse) ── */

    if (id == 1) {
        /*
         * T1: acquire mutex, write early_exit=1, release.
         * Then synchronise with T2 via check_barrier before proceeding
         * to round_barrier so that DPOR explores the T1-before-T3 and
         * T3-before-T1 orderings independently of T1-T2 ordering.
         */
        pthread_mutex_lock(&flag_mutex);
        early_exit = 1;
        printf("[T1] Round 2: wrote early_exit = 1\n");
        pthread_mutex_unlock(&flag_mutex);

        printf("[T1] Round 2: arriving at check_barrier\n");
        pthread_barrier_wait(&check_barrier);
        printf("[T1] Round 2: passed check_barrier\n");

        printf("[T1] Round 2: arriving at round_barrier\n");
        pthread_barrier_wait(&round_barrier);
        printf("[T1] Round 2: passed round_barrier (gen:1)\n");

    } else if (id == 2) {
        /*
         * T2: wait at check_barrier first (ensures T1 has written
         * early_exit before T2 reads it), then read for informational
         * purposes only, then proceed to round_barrier.
         * T2 always reaches round_barrier regardless of early_exit.
         */
        printf("[T2] Round 2: arriving at check_barrier\n");
        pthread_barrier_wait(&check_barrier);
        printf("[T2] Round 2: passed check_barrier\n");

        pthread_mutex_lock(&flag_mutex);
        int val = early_exit;
        printf("[T2] Round 2: read early_exit = %d (info only)\n", val);
        pthread_mutex_unlock(&flag_mutex);

        printf("[T2] Round 2: arriving at round_barrier\n");
        pthread_barrier_wait(&round_barrier);
        printf("[T2] Round 2: passed round_barrier (gen:1)\n");

    } else { /* id == 3 */
        /*
         * T3: read early_exit under the mutex.
         *   - If early_exit == 1: skip round_barrier(gen:1) → DEADLOCK
         *     (only T1 and T2 will arrive; count=3 never satisfied).
         *   - If early_exit == 0: proceed to round_barrier(gen:1) → SAFE.
         *
         * The race: T1 may or may not have written before T3 acquires
         * the mutex.  McMini must explore both orderings.
         */
        pthread_mutex_lock(&flag_mutex);
        int local_val = early_exit;
        printf("[T3] Round 2: read early_exit = %d\n", local_val);
        pthread_mutex_unlock(&flag_mutex);

        if (local_val == 1) {
            printf("[T3] Round 2: early_exit set — returning early "
                   "(causes deadlock!)\n");
            return NULL;
        }

        printf("[T3] Round 2: early_exit not set — arriving at round_barrier\n");
        pthread_barrier_wait(&round_barrier);
        printf("[T3] Round 2: passed round_barrier (gen:1)\n");
    }

    return NULL;
}

int main(void)
{
    pthread_t   threads[NUM_WORKERS];
    worker_arg  args[NUM_WORKERS];

    pthread_barrier_init(&round_barrier, NULL, NUM_WORKERS);
    pthread_barrier_init(&check_barrier, NULL, 2);
    pthread_mutex_init(&flag_mutex, NULL);

    printf("=== Multi-Phase Barrier Deadlock Stress Test ===\n");
    printf("round_barrier count=%d, check_barrier count=2\n", NUM_WORKERS);
    printf("Worker 3 skips round_barrier(gen:1) if early_exit==1 (set by T1).\n\n");

    for (int i = 0; i < NUM_WORKERS; i++) {
        args[i].id = i + 1;
        pthread_create(&threads[i], NULL, worker_fn, &args[i]);
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_barrier_destroy(&round_barrier);
    pthread_barrier_destroy(&check_barrier);
    pthread_mutex_destroy(&flag_mutex);

    printf("\n=== All workers completed. No deadlock. ===\n");
    return 0;
}
