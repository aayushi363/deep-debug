/* Public header for timing/reporting. Implementation lives in
 * src/lib/wrapper_timing.c to keep global state in a single TU.
 */
#ifndef MCMINI_WRAPPER_TIMING_H
#define MCMINI_WRAPPER_TIMING_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifdef __cplusplus
#include <atomic>
#define ATOMIC(T) std::atomic<T>
#define FUNC_NAME __FUNCTION__
#else
#include <stdatomic.h>
#define ATOMIC(T) _Atomic(T)
#define FUNC_NAME __func__
#endif

/* --- Public types --- */
typedef struct TimingResult {
    const char* function_name;
    uint64_t duration_ns;
    uint64_t paused_duration_ns;
    struct TimingResult* next;
} TimingResult;

typedef struct ThreadDataNode {
    struct TimingResult** thread_results_head_ptr;
    struct ThreadDataNode* next;
} ThreadDataNode;

/* TimerInfo is used by the MEASURE_FUNCTION_TIME macro and by record_time */
// typedef struct {
//     const char* function_name;
//     struct timespec start_time;
//     uint64_t accumulated_ns;
//     uint64_t total_paused_ns;
//     int is_paused;
// } TimerInfo;
typedef struct {
    const char* function_name;
    uint64_t start_tsc;
    uint64_t accumulated_cycles;
    uint64_t total_paused_cycles;
    int is_paused;
} TimerInfo;

/* --- TLS variables (declared here, defined in the implementation TU) ---
 * These must be extern so the single implementation TU can read/write them
 * and so they are not duplicated with internal linkage across TUs.
 */
extern __thread TimingResult* thread_local_results_head;
extern __thread int thread_registered;
extern _Atomic(int) mcmini_timing_enabled;

/* --- Public API (implemented in src/lib/wrapper_timing.c) --- */
void timer_init(const char* report_filepath);
void save_timing_report(const char* filepath);
int register_thread_data(struct ThreadDataNode* node);

/* record_time is called by the cleanup attribute; defined in the .c */
void record_time(TimerInfo* info);

/* Declaration of the real pthread_create in the underlying libpthread so
 * we can create helper threads without the mc_pthread_create wrapper
 * recording them. The interception layer provides `libpthread_pthread_create`.
 */
extern int libpthread_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                    void *(*start_routine)(void *), void *arg);

/* Helper: get current time. Implemented inline since it's trivial and
 * independent of global state.
 */
// static inline struct timespec get_current_time(void) {
//     struct timespec ts;
//     /* Use a raw syscall for clock_gettime to avoid interception by DMTCP's
//      * wrapper (which may take locks). Using the syscall here prevents
//      * re-entering DMTCP wrapper machinery during signal/quiesce phases
//      * such as PRESUSPEND.
//      */
//     long rc = syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
//     if (rc != 0) {
//         /* Fallback to libc if syscall fails for some reason. */
//         clock_gettime(CLOCK_REALTIME, &ts);
//     }
//     return ts;
// }

/* Single x86 instruction, ~1-2 ns. DMTCP cannot intercept it.
 * Used for duration measurement only — not wall-clock time. */
static inline uint64_t get_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Macro used in functions to measure wall-clock time. Uses a cleanup
 * function (record_time) implemented in the .c file.
 */

#define MEASURE_FUNCTION_TIME \
    int _tmg = atomic_load_explicit(&mcmini_timing_enabled, memory_order_relaxed); \
    __attribute__((cleanup(record_time))) \
    TimerInfo timer_info_var = { .function_name = _tmg ? FUNC_NAME : NULL, \
                                 .start_tsc = _tmg ? get_rdtsc() : 0, \
                                 .accumulated_cycles = 0, \
                                 .total_paused_cycles = 0, \
                                 .is_paused = 0 };

/* Pause/resume helpers that update the TimerInfo fields. */

#define PAUSE_TIMER \
    if (_tmg && !timer_info_var.is_paused) { \
        uint64_t _p_now = get_rdtsc(); \
        timer_info_var.accumulated_cycles += _p_now - timer_info_var.start_tsc; \
        timer_info_var.start_tsc = _p_now; \
        timer_info_var.is_paused = 1; \
    }

#define RESUME_TIMER \
    if (_tmg && timer_info_var.is_paused) { \
        uint64_t _r_now = get_rdtsc(); \
        timer_info_var.total_paused_cycles += _r_now - timer_info_var.start_tsc; \
        timer_info_var.start_tsc = _r_now; \
        timer_info_var.is_paused = 0; \
    }

#endif /* MCMINI_WRAPPER_TIMING_H */
