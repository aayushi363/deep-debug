// input_deadlock.c
//
// AB/BA deadlock parameterized by INPUT the fuzzer can vary.
//
// The classic ab_ba_deadlock.c has two threads that ALWAYS acquire mutexes
// in opposite orders — the deadlock class is reachable from `main` without
// any input variation, so mcmini finds it in one enumeration and the
// fuzzer has nothing meaningful to fuzz.
//
// This variant reads a command list from argv. Each command is 'A' or 'B':
//   'A' → acquire lockA then lockB (lock order A→B)
//   'B' → acquire lockB then lockA (lock order B→A)
//
// The command list is split in half: first half runs on thread 1, second
// half on thread 2. The deadlock class is reachable only when thread 1's
// commands include some 'A' AND thread 2's commands include some 'B'
// (or vice versa) — otherwise both threads use the same lock order and
// no deadlock is possible.
//
// Composition value:
//   - Fuzzer's contribution: varies argv to explore which command mixes
//     produce a deadlock-reachable configuration. Many inputs are safe
//     (e.g. all 'A'); some create the AB/BA hazard.
//   - Mcmini's contribution: for a given input, DPOR enumerates the
//     schedule space. Deadlock-reachable inputs produce a deadlock class.
//     Deadlock-unreachable inputs produce only clean classes.
//
// Usage:
//   ./input_deadlock A A A A         → no deadlock possible (both threads use A→B)
//   ./input_deadlock B B B B         → no deadlock possible (both use B→A)
//   ./input_deadlock A A B B         → deadlock possible (T1: A→B, T2: B→A)
//   ./input_deadlock A B A B         → deadlock possible (mixed both threads)

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lockA, lockB;

typedef struct {
    int id;
    int ncmds;
    const char *cmds;
} thread_args_t;

static void *worker(void *p) {
    thread_args_t *a = (thread_args_t *)p;
    for (int i = 0; i < a->ncmds; i++) {
        char c = a->cmds[i];
        if (c == 'A') {
            fprintf(stderr, "[T%d] cmd A: lock A\n", a->id);
            pthread_mutex_lock(&lockA);
            fprintf(stderr, "[T%d] cmd A: lock B\n", a->id);
            pthread_mutex_lock(&lockB);
            fprintf(stderr, "[T%d] cmd A: release\n", a->id);
            pthread_mutex_unlock(&lockB);
            pthread_mutex_unlock(&lockA);
        } else if (c == 'B') {
            fprintf(stderr, "[T%d] cmd B: lock B\n", a->id);
            pthread_mutex_lock(&lockB);
            fprintf(stderr, "[T%d] cmd B: lock A\n", a->id);
            pthread_mutex_lock(&lockA);
            fprintf(stderr, "[T%d] cmd B: release\n", a->id);
            pthread_mutex_unlock(&lockA);
            pthread_mutex_unlock(&lockB);
        } else {
            fprintf(stderr, "[T%d] unknown cmd '%c', skipping\n", a->id, c);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s cmd1 cmd2 ...\n", argv[0]);
        fprintf(stderr, "  each cmd is 'A' (lock A then B) or 'B' (lock B then A)\n");
        fprintf(stderr, "  first half of commands runs on thread 1, second half on thread 2\n");
        return 1;
    }

    // Send this JSON to a certain file:
    const char* message = "{ \"antithesis_setup\": { \"status\": \"complete\", \"details\": null } }";
    const char* path = getenv("ANTITHESIS_OUTPUT_DIR");
    char filename[1000];
    sprintf(filename, "%s/mcmini.json", path);
    FILE* event_stream_file = fopen(filename, "w");
    fprintf(event_stream_file, "%s\n", message);
    fflush(event_stream_file);


    int total = argc - 1;
    int n1 = total / 2;
    int n2 = total - n1;

    char *cmds1 = calloc(n1 + 1, 1);
    char *cmds2 = calloc(n2 + 1, 1);
    if (!cmds1 || !cmds2) { fprintf(stderr, "OOM\n"); return 1; }
    for (int i = 0; i < n1; i++) cmds1[i] = argv[1 + i][0];
    for (int i = 0; i < n2; i++) cmds2[i] = argv[1 + n1 + i][0];

    fprintf(stderr, "[main] thread 1 cmds: '%s'\n", cmds1);
    fprintf(stderr, "[main] thread 2 cmds: '%s'\n", cmds2);

    pthread_mutex_init(&lockA, NULL);
    pthread_mutex_init(&lockB, NULL);

    thread_args_t a1 = { .id = 1, .ncmds = n1, .cmds = cmds1 };
    thread_args_t a2 = { .id = 2, .ncmds = n2, .cmds = cmds2 };

    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker, &a1);
    pthread_create(&t2, NULL, worker, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    pthread_mutex_destroy(&lockA);
    pthread_mutex_destroy(&lockB);
    free(cmds1);
    free(cmds2);
    fprintf(stderr, "[main] done\n");
    return 0;
}
  