// Classic AB/BA deadlock with a pure-CPU wall-clock-bounded warmup.
//
// Two threads acquire two mutexes in opposite orders. Depending on the
// interleaving they may complete cleanly or deadlock. DPOR's job is to
// enumerate the non-equivalent interleavings.
//
// Why no sleep/usleep in the warmup: mcmini intercepts those in model-checking
// mode and returns immediately. clock_gettime is NOT intercepted, so we use
// it to bound a pure CPU busy-loop.
//
// Why a PURE-CPU warmup (no mutex ops during warmup): the warmup's job is to
// take wall-clock time so DMTCP can land a checkpoint BEFORE the deadlock
// fixture. It must NOT add visible pthread operations to the recorded trace,
// because every recorded op adds depth to DPOR's search tree. With a CPU-
// only warmup, after a phase-2 checkpoint restore the model checker sees
// only the AB/BA fixture — so deadlock-finding interleavings appear within
// DPOR's first few traces.

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static pthread_mutex_t lock_a;
static pthread_mutex_t lock_b;

// Default 3 seconds of wall-clock — long enough for DMTCP (at -i 2) to take
// a checkpoint before the deadlock phase. Override via MCMINI_WARMUP_SECONDS.
static int warmup_seconds(void) {
    const char *e = getenv("MCMINI_WARMUP_SECONDS");
    int n = e ? atoi(e) : 3;
    return n > 0 ? n : 3;
}

// Pure CPU busy-wait. Produces ZERO pthread operations during the warmup so
// DPOR's post-checkpoint search tree starts at the AB/BA fixture.
static void warmup(void) {
    int target = warmup_seconds();
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return;
    volatile long sink = 0;
    long iter = 0;
    while (1) {
        sink += iter;
        iter++;
        // Check elapsed time every ~1M iterations to amortize the syscall.
        if ((iter & 0xFFFFF) == 0) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) continue;
            if (now.tv_sec - start.tv_sec >= target) break;
        }
    }
}

static void *thread_ab(void *arg) {
    (void)arg;
    fprintf(stderr, "[T-AB] warmup start\n");
    warmup();
    fprintf(stderr, "[T-AB] warmup done, locking A\n");
    pthread_mutex_lock(&lock_a);
    fprintf(stderr, "[T-AB] locked A, locking B\n");
    pthread_mutex_lock(&lock_b);
    fprintf(stderr, "[T-AB] locked B (no deadlock on this interleaving)\n");
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
    return NULL;
}

static void *thread_ba(void *arg) {
    (void)arg;
    fprintf(stderr, "[T-BA] warmup start\n");
    warmup();
    fprintf(stderr, "[T-BA] warmup done, locking B\n");
    pthread_mutex_lock(&lock_b);
    fprintf(stderr, "[T-BA] locked B, locking A\n");
    pthread_mutex_lock(&lock_a);
    fprintf(stderr, "[T-BA] locked A (no deadlock on this interleaving)\n");
    pthread_mutex_unlock(&lock_a);
    pthread_mutex_unlock(&lock_b);
    return NULL;
}

int main(void) {
    pthread_mutex_init(&lock_a, NULL);
    pthread_mutex_init(&lock_b, NULL);
    pthread_t t_ab, t_ba;
    pthread_create(&t_ab, NULL, thread_ab, NULL);
    pthread_create(&t_ba, NULL, thread_ba, NULL);
    pthread_join(t_ab, NULL);
    pthread_join(t_ba, NULL);
    pthread_mutex_destroy(&lock_a);
    pthread_mutex_destroy(&lock_b);
    return 0;
}
