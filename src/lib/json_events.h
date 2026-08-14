#ifndef DEEP_DEBUG_JSON_EVENTS_H
#define DEEP_DEBUG_JSON_EVENTS_H
#include <stdint.h>
// Thread-local reentry guard. Set to 1 while a thread is inside a libmcmini
// wrapper's FUZZER_STANDALONE branch. Prevents recursion when stdio-internal
// flockfile() → pthread_mutex_lock → our wrapper → getchar → flockfile → ...
extern __thread int mc_in_wrapper;
// Emit one pthread event as a JSON line to $ANTITHESIS_OUTPUT_DIR/mcmini.json.
// Thread-safe via internal mutex. File opened on first call, kept open for the
// process lifetime. Silently no-ops if $ANTITHESIS_OUTPUT_DIR is unset.
void write_pthread_event(const char *event_name,
                         uint32_t thread_id,
                         uint32_t object_id,
                         uint64_t seqno);

uint32_t get_json_thread_id(void);   // dense thread id, first-touch atomic
uint64_t next_json_seqno(void);      // global monotonic per-process

// Provided at runtime by libvoidstar.so / libinstrumentation_determ.so.
extern int fuzz_getchar(void);

#endif