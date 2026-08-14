#include "json_events.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Bypass our own pthread interceptors — otherwise write_pthread_event
// recurses via mc_pthread_mutex_lock → write_pthread_event → ...
extern int libpthread_mutex_lock(pthread_mutex_t *mutex);
extern int libpthread_mutex_unlock(pthread_mutex_t *mutex);

// Emits the "MCMINI_SCHEDULE_JSON: {...}" schedule summary on stderr — the
// only line DeepDebugStrategy parses (deep_debug_strategy.rs:44). Defined in
// dmtcp-callback.c. We call it from the abort path so the coordinator-driven
// outcome is recorded as a schedule class, not just printed as a human line.
extern void mcmini_emit_phase1_stub(void);


static FILE *g_out = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint_least64_t g_seqno = 0;
static atomic_uint_least32_t g_thread_counter = 0;

static __thread uint32_t t_thread_id = 0;
static __thread int      t_thread_id_initialized = 0;
__thread int mc_in_wrapper = 0;

uint32_t get_json_thread_id(void) {
    if (!t_thread_id_initialized) {
        t_thread_id = atomic_fetch_add_explicit(
            &g_thread_counter, 1, memory_order_relaxed);
        t_thread_id_initialized = 1;
    }
    return t_thread_id;
}

uint64_t next_json_seqno(void) {
    return atomic_fetch_add_explicit(&g_seqno, 1, memory_order_relaxed);
}

static void open_output_file_locked(void) {
    if (g_out) return;
    const char *dir = getenv("ANTITHESIS_OUTPUT_DIR");
    // fprintf(stderr, "[DEBUG] open_output_file_locked: ANTITHESIS_OUTPUT_DIR=%s\n",
    //         dir ? dir : "(unset)");
    // fflush(stderr);
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/mcmini.json", dir);
    g_out = fopen(path, "a");
    fprintf(stderr, "[DEBUG] open_output_file_locked: fopen(\"%s\", \"a\") returned %p (errno=%d %s)\n",
            path, (void*)g_out, errno, g_out ? "OK" : strerror(errno));
    fflush(stderr);
}

void write_pthread_event(const char *event_name,
                         uint32_t thread_id,
                         uint32_t object_id,
                         uint64_t seqno) {
    libpthread_mutex_lock(&g_mutex);
    open_output_file_locked();
    if (g_out) {
        fprintf(g_out,
            "{\"event\":\"%s\",\"thread\":%u,\"object\":\"0x%08x\",\"seqno\":%llu}\n",
            event_name, thread_id, object_id, (unsigned long long)seqno);
        fflush(g_out);
    }
    libpthread_mutex_unlock(&g_mutex);
}