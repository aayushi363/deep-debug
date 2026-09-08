/* Central implementation for timing/report saving.
 * Keeps global list and watcher in a single translation unit to avoid
 * duplicated static state across TUs.
 */
#include "../..//include/mcmini/wrapper_timing.h"
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>

/* Raw syscall helpers using the `syscall` instruction directly so we don't hit
 * any libc-level wrappers that DMTCP may have LD_PRELOAD'd. These are at
 * file scope (not inside a function) so they compile cleanly.
 */
static inline long raw_openat(int dirfd, const char *pathname, int flags, mode_t mode) {
    long ret;
    register long rax __asm__("rax") = SYS_openat;
    register long rdi __asm__("rdi") = dirfd;
    register const char *rsi __asm__("rsi") = pathname;
    register long rdx __asm__("rdx") = flags;
    register long r10 __asm__("r10") = mode;
    __asm__ volatile ("syscall"
                      : "=a" (ret)
                      : "r" (rax), "r" (rdi), "r" (rsi), "r" (rdx), "r" (r10)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline long raw_write(int fd, const void *buf, size_t count) {
    long ret;
    register long rax __asm__("rax") = SYS_write;
    register long rdi __asm__("rdi") = fd;
    register const void *rsi __asm__("rsi") = buf;
    register long rdx __asm__("rdx") = (long)count;
    __asm__ volatile ("syscall"
                      : "=a" (ret)
                      : "r" (rax), "r" (rdi), "r" (rsi), "r" (rdx)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline long raw_close(int fd) {
    long ret;
    register long rax __asm__("rax") = SYS_close;
    register long rdi __asm__("rdi") = fd;
    __asm__ volatile ("syscall"
                      : "=a" (ret)
                      : "r" (rax), "r" (rdi)
                      : "rcx", "r11", "memory");
    return ret;
}

/* DMTCP wraps the glibc syscall() function via LD_PRELOAD, so
 * syscall(SYS_exit_group, ...) can be intercepted and suppressed.
 * Use the same inline-asm pattern as raw_write/raw_close to bypass it. */
static inline __attribute__((noreturn)) void raw_exit_group(int code) {
    register long rax __asm__("rax") = SYS_exit_group;
    register long rdi __asm__("rdi") = (long)code;
    __asm__ volatile ("syscall"
                      : "+a" (rax)
                      : "r" (rdi)
                      : "rcx", "r11", "memory");
    __builtin_unreachable();
}

/* Global list head kept in this TU only. */
static ATOMIC(ThreadDataNode*) g_all_threads_list_head = ATOMIC_VAR_INIT(NULL);
static const char* g_report_filepath = "/tmp/timing_report.txt";
static volatile sig_atomic_t g_signal_received = 0;
static ATOMIC(int) report_saved = ATOMIC_VAR_INIT(0);
/* Gate: set to 1 when MCMINI_ENABLE_TIMING env var is present. */
_Atomic(int) mcmini_timing_enabled = ATOMIC_VAR_INIT(0);

/* TSC cycles per second, calibrated once at timer_init(). */
static uint64_t g_tsc_freq_hz = 3000000000ULL;

/* Pre-allocated pool size per thread — matches existing cap. */
#define TIMING_POOL_SIZE 100000

/* Definitions for TLS variables declared in the public header. These must
 * be defined in a single TU so record_time and the rest of the implementation
 * operate on the same thread-local storage.
 */
__thread TimingResult* thread_local_results_head = NULL;
__thread int thread_registered = 0;
static __thread TimingResult *result_pool      = NULL;
static __thread uint32_t     result_pool_used  = 0;

/* Forward declarations */
static void final_report_and_exit(int signum);
static void *timing_report_watcher(void *arg);
extern int libpthread_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                    void *(*start_routine)(void *), void *arg);

int register_thread_data(struct ThreadDataNode* node) {
    if (!node) return -1;
    ThreadDataNode* old_head = atomic_load(&g_all_threads_list_head);
    do {
        node->next = old_head;
    } while (!atomic_compare_exchange_weak(&g_all_threads_list_head, &old_head, node));
    return 0;
}

static void final_report_and_exit(int signum) {
    g_signal_received = signum;
    const char msg[] = "[TIMER INFO] Signal received, scheduling report save\n";
    write(STDERR_FILENO, msg, sizeof(msg)-1);
    /* For user-initiated signals (Ctrl+C, kill), exit immediately.
     * Saving the report can take tens of seconds when there are millions of
     * per-call timing records. For crash signals (SIGSEGV, SIGABRT) we still
     * fall through to let the watcher thread save before exiting. */
    if (signum == SIGINT || signum == SIGTERM) {
        raw_exit_group(128 + signum);
    }
}

void timer_init(const char* report_filepath) {
    const char *_tmg_env = getenv("MCMINI_ENABLE_TIMING");
    if (_tmg_env != NULL && _tmg_env[0] != '0' && _tmg_env[0] != '\0') {
        atomic_store(&mcmini_timing_enabled, 1);

        FILE *cf = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
        if (cf) {
            unsigned long khz = 0;
            if (fscanf(cf, "%lu", &khz) == 1 && khz > 0)
                g_tsc_freq_hz = (uint64_t)khz * 1000ULL;
            fclose(cf);
        }

        if (report_filepath) g_report_filepath = report_filepath;
        fprintf(stderr, "[TIMER INFO] Timer initialized. Report will be saved to '%s'.\n", g_report_filepath);
    }

    signal(SIGINT,  final_report_and_exit);
    signal(SIGTERM, final_report_and_exit);
    signal(SIGSEGV, final_report_and_exit);
    signal(SIGABRT, final_report_and_exit);

    pthread_t watcher;
    if (libpthread_pthread_create(&watcher, NULL, timing_report_watcher, NULL) == 0) {
        pthread_detach(watcher);
    } else {
        if (pthread_create(&watcher, NULL, timing_report_watcher, NULL) == 0) {
            pthread_detach(watcher);
        }
    }
}

void save_timing_report(const char* filepath) {
    if (!atomic_load_explicit(&mcmini_timing_enabled, memory_order_relaxed)) return;  
    if (atomic_exchange(&report_saved, 1) != 0) return;
    const char *usepath = filepath ? filepath : g_report_filepath;

    /* Print info to stderr (this may be wrapped, but it's just for humans). */
    fprintf(stderr, "[TIMER INFO] Attempting to save report to '%s'...\n", usepath);

    /*
     * Use direct syscalls (openat/write/close) to avoid DMTCP wrappers that
     * call into thread-registration logic. We format each line into a stack
     * buffer and write via syscall(SYS_write,...).
     */
    #include <sys/syscall.h>
    int fd = (int)raw_openat(AT_FDCWD, usepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[TIMER ERROR] Could not open report file '%s'. Reason: %s\n",
                usepath, strerror(errno));
        return;
    }

    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "--- Wall Clock Timing Report ---\n");
    raw_write(fd, buf, (size_t)len);
    len = snprintf(buf, sizeof(buf), "Note: Times shown are wall clock times (includes I/O, sleep, etc.)\n\n");
    raw_write(fd, buf, (size_t)len);

    ThreadDataNode* current_thread_node = atomic_load(&g_all_threads_list_head);
    while (current_thread_node != NULL) {
        TimingResult* current_result = *current_thread_node->thread_results_head_ptr;
        while (current_result != NULL) {
            len = snprintf(buf, sizeof(buf), "Call to '%s' took: %llu ns\n",
                           current_result->function_name,
                           (unsigned long long)current_result->duration_ns);
            raw_write(fd, buf, (size_t)len);
            current_result = current_result->next;
        }
        current_thread_node = current_thread_node->next;
    }

    len = snprintf(buf, sizeof(buf), "--- End of Report ---\n");
    raw_write(fd, buf, (size_t)len);
    raw_close(fd);

    fprintf(stderr, "[TIMER INFO] Report successfully saved.\n");
}

static void *timing_report_watcher(void *arg) {
    struct timespec sleep_ts = {0, 10000000L}; /* 10ms */
    while (1) {
        if (g_signal_received) {
            save_timing_report(NULL);
            /* raw_exit_group bypasses DMTCP's LD_PRELOAD wrapper around the
             * glibc syscall() function, which suppresses SYS_exit_group to
             * prevent uncoordinated teardown from inside the child. */
            raw_exit_group(128 + (int)g_signal_received);
        }
        nanosleep(&sleep_ts, NULL);
    }
    return NULL;
}

/* record_time implementation used by the cleanup attribute. */
// void record_time(TimerInfo* info) {
//     if (!info) return;
//     uint64_t final_duration_ns = info->accumulated_ns;
//     if (!info->is_paused) {
//         struct timespec end_time = get_current_time();
//         final_duration_ns += (end_time.tv_sec - info->start_time.tv_sec) * 1000000000ULL +
//                              (end_time.tv_nsec - info->start_time.tv_nsec);
//     }
//     if (final_duration_ns == 0 && info->total_paused_ns == 0) return;

//     /* Cap per-thread records so DMTCP checkpoint size stays bounded.
//      * Without this cap, O(N log N) mutex calls (e.g. Barnes with 512K
//      * particles) accumulate gigabytes of linked-list nodes that DMTCP
//      * must snapshot on every checkpoint interval. */
//     static __thread size_t thread_record_count = 0;
//     if (thread_record_count >= 100000) return;
//     thread_record_count++;

//     if (!thread_registered) {
//         ThreadDataNode* new_thread_node = (ThreadDataNode*)malloc(sizeof(ThreadDataNode));
//         if (!new_thread_node) return;
//         new_thread_node->thread_results_head_ptr = &thread_local_results_head;
//         new_thread_node->next = NULL;
//         if (register_thread_data(new_thread_node) != 0) {
//             free(new_thread_node);
//             return;
//         }
//         thread_registered = 1;
//     }

//     TimingResult* new_result = (TimingResult*)malloc(sizeof(TimingResult));
//     if (!new_result) return;
//     new_result->function_name = info->function_name;
//     new_result->duration_ns = final_duration_ns;
//     new_result->paused_duration_ns = info->total_paused_ns;
//     new_result->next = thread_local_results_head;
//     thread_local_results_head = new_result;
// }
/* record_time implementation used by the cleanup attribute. */
void record_time(TimerInfo* info) {
    /* Gate: start_tsc == 0 means timing was disabled when this frame started. */
    if (!info || info->start_tsc == 0) return;

    /* Compute total cycles, excluding time spent inside real pthread calls. */
    uint64_t final_cycles = info->accumulated_cycles;
    if (!info->is_paused)
        final_cycles += get_rdtsc() - info->start_tsc;
    if (final_cycles == 0 && info->total_paused_cycles == 0) return;

    /* Convert cycles → ns using TSC frequency calibrated at init. */
    uint64_t final_duration_ns    = (final_cycles * 1000000000ULL) / g_tsc_freq_hz;
    uint64_t paused_duration_ns   = (info->total_paused_cycles * 1000000000ULL) / g_tsc_freq_hz;

    /* One-time per-thread setup: allocate pool + register with global list. */
    if (!thread_registered) {
        result_pool = (TimingResult*)malloc(TIMING_POOL_SIZE * sizeof(TimingResult));
        if (!result_pool) return;

        ThreadDataNode *node = (ThreadDataNode*)malloc(sizeof(ThreadDataNode));
        if (!node) { free(result_pool); result_pool = NULL; return; }
        node->thread_results_head_ptr = &thread_local_results_head;
        node->next = NULL;
        if (register_thread_data(node) != 0) {
            free(node); free(result_pool); result_pool = NULL; return;
        }
        thread_registered = 1;
    }

    /* Pool full — stop recording for this thread. */
    if (!result_pool || result_pool_used >= TIMING_POOL_SIZE) return;

    /* Bump-allocate from pre-allocated pool: no malloc on the hot path. */
    TimingResult *r       = &result_pool[result_pool_used++];
    r->function_name      = info->function_name;
    r->duration_ns        = final_duration_ns;
    r->paused_duration_ns = paused_duration_ns;
    r->next               = thread_local_results_head;
    thread_local_results_head = r;
}
