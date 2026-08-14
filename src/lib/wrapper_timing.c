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

/* Phase 1: defined in dmtcp-callback.c. Called from the watcher thread on the
 * signal-termination exit path (which uses _exit() and would otherwise bypass
 * atexit handlers, hiding our schedule emit). */
extern void mcmini_emit_phase1_stub(void);

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

/* Global list head kept in this TU only. */
static ATOMIC(ThreadDataNode*) g_all_threads_list_head = ATOMIC_VAR_INIT(NULL);
static const char* g_report_filepath = "/tmp/timing_report.txt";
static volatile sig_atomic_t g_signal_received = 0;
static ATOMIC(int) report_saved = ATOMIC_VAR_INIT(0);

/* Definitions for TLS variables declared in the public header. These must
 * be defined in a single TU so record_time and the rest of the implementation
 * operate on the same thread-local storage.
 */
__thread TimingResult* thread_local_results_head = NULL;
__thread int thread_registered = 0;

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
    const char msg[] = "[TIMER INFO] Signal received, exiting\n";
    write(STDERR_FILENO, msg, sizeof(msg)-1);
}

void timer_init(const char* report_filepath) {
    if (report_filepath) g_report_filepath = report_filepath;

    signal(SIGINT, final_report_and_exit);
    signal(SIGTERM, final_report_and_exit);
    signal(SIGSEGV, final_report_and_exit);
    signal(SIGABRT, final_report_and_exit);
    fprintf(stderr, "[TIMER INFO] Timer initialized. Report will be saved to '%s'.\n", g_report_filepath);

    pthread_t watcher;
    if (libpthread_pthread_create(&watcher, NULL, timing_report_watcher, NULL) == 0) {
        pthread_detach(watcher);
    } else {
        /* Fallback: try normal pthread_create */
        if (pthread_create(&watcher, NULL, timing_report_watcher, NULL) == 0) {
            pthread_detach(watcher);
        }
    }
}

void save_timing_report(const char* filepath) {
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
            /* Emit the schedule stub on the signal-terminated exit path.
             * Without this, _exit() below would skip the atexit-registered
             * handler in dmtcp-callback.c. The function is idempotent.
             *
             * save_timing_report intentionally omitted: it lengthens the
             * Ctrl+C / signal exit path and the TIMER_INFO diagnostic file
             * isn't load-bearing for phase 1. */
            mcmini_emit_phase1_stub();
            _exit(128 + (int)g_signal_received);
        }
        nanosleep(&sleep_ts, NULL);
    }
    return NULL;
}

/* record_time implementation used by the cleanup attribute. */
void record_time(TimerInfo* info) {
    if (!info) return;
    uint64_t final_duration_ns = info->accumulated_ns;
    if (!info->is_paused) {
        struct timespec end_time = get_current_time();
        final_duration_ns += (end_time.tv_sec - info->start_time.tv_sec) * 1000000000ULL +
                             (end_time.tv_nsec - info->start_time.tv_nsec);
    }
    if (final_duration_ns == 0 && info->total_paused_ns == 0) return;

    if (!thread_registered) {
        ThreadDataNode* new_thread_node = (ThreadDataNode*)malloc(sizeof(ThreadDataNode));
        if (!new_thread_node) return;
        new_thread_node->thread_results_head_ptr = &thread_local_results_head;
        new_thread_node->next = NULL;
        if (register_thread_data(new_thread_node) != 0) {
            free(new_thread_node);
            return;
        }
        thread_registered = 1;
    }

    TimingResult* new_result = (TimingResult*)malloc(sizeof(TimingResult));
    if (!new_result) return;
    new_result->function_name = info->function_name;
    new_result->duration_ns = final_duration_ns;
    new_result->paused_duration_ns = info->total_paused_ns;
    new_result->next = thread_local_results_head;
    thread_local_results_head = new_result;
}
