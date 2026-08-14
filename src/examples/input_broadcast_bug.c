// input_broadcast_bug.c
//
// signal-vs-broadcast bug parameterized by input.
//
// N consumer threads each wait on a shared condition variable for "ready".
// One producer thread sets ready=1 and calls pthread_cond_signal (wakes ONE
// waiter). The buggy code should have used pthread_cond_broadcast (wakes ALL).
//
// If the fuzzer-side input is:
//   N=1 → no deadlock (single consumer wakes on the single signal)
//   N=2 → one consumer wakes, one is stranded → deadlock
//   N=3 → two consumers stranded → deadlock
//   ...
//
// Composition value:
//   - Fuzzer's contribution: chooses N. Some N are safe (N=1), others are
//     buggy (N>=2). Distinguishing safe vs unsafe requires varying N.
//   - Mcmini's contribution: for a given N>=2, DPOR confirms that no
//     schedule avoids the stranded-waiter deadlock. For N=1, DPOR
//     confirms all schedules complete cleanly.
//
// Different bug shape from input_deadlock (AB/BA) and input_lost_signal
// (lost wakeup): here the bug is a *design* mistake (signal vs broadcast)
// whose severity depends only on N. Schedule variation doesn't help the
// fuzzer avoid it — the deadlock is baked in by input.
//
// Usage:
//   ./input_broadcast_bug 1    → no deadlock
//   ./input_broadcast_bug 2    → deadlock (one consumer stranded)
//   ./input_broadcast_bug 3    → deadlock (two consumers stranded)

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t mtx;
static pthread_cond_t cv;
static int ready_flag = 0;

static void *consumer(void *arg) {
    long id = (long)arg;
    pthread_mutex_lock(&mtx);
    while (ready_flag == 0) {
        fprintf(stderr, "[C%ld] wait\n", id);
        pthread_cond_wait(&cv, &mtx);
    }
    fprintf(stderr, "[C%ld] proceed\n", id);
    pthread_mutex_unlock(&mtx);
    return NULL;
}

static void *producer(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mtx);
    ready_flag = 1;
    fprintf(stderr, "[P] signal (BUG: should be broadcast)\n");
    // BUG: signal wakes only ONE waiter. If more than one consumer is
    // waiting, the others remain blocked forever.
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mtx);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s N   (N = number of consumer threads)\n", argv[0]);
        return 1;
    }
    int n = atoi(argv[1]);
    if (n < 1) n = 1;

    pthread_mutex_init(&mtx, NULL);
    pthread_cond_init(&cv, NULL);

    pthread_t *consumers = calloc(n, sizeof(pthread_t));
    if (!consumers) { fprintf(stderr, "OOM\n"); return 1; }
    pthread_t p_thread;

    fprintf(stderr, "[main] spawning %d consumer(s) and 1 producer\n", n);

    // Spawn all consumers first so they can be waiting when producer signals.
    for (int i = 0; i < n; i++) {
        pthread_create(&consumers[i], NULL, consumer, (void *)(long)(i + 1));
    }
    pthread_create(&p_thread, NULL, producer, NULL);

    for (int i = 0; i < n; i++) pthread_join(consumers[i], NULL);
    pthread_join(p_thread, NULL);

    pthread_mutex_destroy(&mtx);
    pthread_cond_destroy(&cv);
    free(consumers);
    fprintf(stderr, "[main] done\n");
    return 0;
}
