// input_lost_signal.c
//
// Lost-wakeup / signal-before-wait bug parameterized by input.
//
// N producer threads (N read from argv[1]) each signal a condition variable.
// One consumer thread waits on that condition variable. Producers signal
// WITHOUT holding the mutex — a classical bug pattern that permits the
// signal(s) to fire before the consumer reaches pthread_cond_wait().
//
// If the fuzzer-side input is:
//   N=0 → no producer exists → consumer waits forever (guaranteed deadlock)
//   N=1 → one producer; deadlock iff producer's signal fires BEFORE consumer's
//         cond_wait (lost wakeup). Requires a specific interleaving.
//   N=2 → two producers; at least one signal has a chance to be "captured"
//         by the consumer, reducing (but not eliminating) the deadlock class.
//   N>=3 → deadlock class becomes rare — most schedules have a signal after
//         the wait.
//
// Composition value:
//   - Fuzzer's contribution: chooses N. Some N produce deadlock-reachable
//     configurations (N=0, N=1); others reduce the class weight.
//   - Mcmini's contribution: for each N, DPOR enumerates the interleavings
//     between producer signal and consumer wait. Discovers the lost-wakeup
//     schedule that a random-scheduling fuzzer would rarely hit.
//
// Usage:
//   ./input_lost_signal 0    → guaranteed deadlock (no signal source)
//   ./input_lost_signal 1    → deadlock possible under lost-wakeup schedule
//   ./input_lost_signal 2    → deadlock possible but less likely
//   ./input_lost_signal 3    → deadlock rare

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t mtx;
static pthread_cond_t cv;
static int ready_flag = 0;

static void *producer(void *arg) {
    long id = (long)arg;
    // BUG: not holding the mutex while updating ready_flag and signaling.
    // Under a schedule where all producers fire before the consumer reaches
    // pthread_cond_wait, the signals are lost.
    ready_flag = 1;
    fprintf(stderr, "[P%ld] signal\n", id);
    pthread_cond_signal(&cv);
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    fprintf(stderr, "[C] lock\n");
    pthread_mutex_lock(&mtx);
    // Buggy consumer: checks ready_flag under lock but does NOT re-check
    // after wakeup. If ready_flag is already 1 before the check, consumer
    // proceeds. If ready_flag is 0 at check but a signal has already fired,
    // pthread_cond_wait will block forever (lost wakeup).
    if (ready_flag == 0) {
        fprintf(stderr, "[C] wait\n");
        pthread_cond_wait(&cv, &mtx);
    }
    fprintf(stderr, "[C] proceed\n");
    pthread_mutex_unlock(&mtx);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s N   (N = number of producer threads)\n", argv[0]);
        return 1;
    }
    int n = atoi(argv[1]);
    if (n < 0) n = 0;

    pthread_mutex_init(&mtx, NULL);
    pthread_cond_init(&cv, NULL);

    pthread_t *producers = NULL;
    if (n > 0) {
        producers = calloc(n, sizeof(pthread_t));
        if (!producers) { fprintf(stderr, "OOM\n"); return 1; }
    }
    pthread_t c_thread;

    fprintf(stderr, "[main] spawning %d producer(s) and 1 consumer\n", n);

    // Spawn producers first so they can potentially race the consumer.
    for (int i = 0; i < n; i++) {
        pthread_create(&producers[i], NULL, producer, (void *)(long)(i + 1));
    }
    pthread_create(&c_thread, NULL, consumer, NULL);

    for (int i = 0; i < n; i++) pthread_join(producers[i], NULL);
    pthread_join(c_thread, NULL);

    pthread_mutex_destroy(&mtx);
    pthread_cond_destroy(&cv);
    free(producers);
    fprintf(stderr, "[main] done\n");
    return 0;
}
