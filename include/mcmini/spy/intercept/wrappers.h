#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcmini/defines.h"
#include "mcmini/lib/entry.h"
#include "mcmini/real_world/mailbox/runner_mailbox.h"
#include "mcmini/spy/checkpointing/record.h"
#include "mcmini/spy/checkpointing/rec_list.h"
#include "mcmini/spy/checkpointing/objects.h"

// See definition in wrappers.c: set while libmcmini creates one of its own
// helper threads, so mc_pthread_create creates it plainly (TSAN-visible,
// DMTCP-known) instead of as a model-checked user thread.
extern MCMINI_THREAD_LOCAL int mc_creating_internal_thread;

void thread_await_scheduler(void);
void thread_wake_scheduler_and_wait(void);
void thread_awake_scheduler_for_thread_finish_transition(void);
void thread_handle_after_dmtcp_restart(void);
volatile runner_mailbox *thread_get_mailbox(void);

// Whether `t` is one of the threads DMTCP resurrected via libc_clone() +
// setcontext() (see restart_child_threads_fast()), rather than one created
// normally after restart. Used by mc_pthread_exit() -- see that function.
bool mc_pthread_is_recreated_thread(pthread_t t);

int mc_pthread_mutex_init(pthread_mutex_t *mutex,
                          const pthread_mutexattr_t *mutexattr);
int mc_pthread_mutex_lock(pthread_mutex_t *mutex);
int mc_pthread_mutex_unlock(pthread_mutex_t *mutex);
int mc_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                    void *(*routine)(void *), void *arg);
int mc_sem_post(sem_t *);
int mc_sem_wait(sem_t *);
int mc_pthread_join(pthread_t, void**);
// See mc_pthread_join_impl() in wrappers.c: like mc_pthread_join(), but
// whenever a real join is warranted, defers the actual join call to the
// caller (setting *deferred = true) instead of performing it via
// libpthread_pthread_join(). For use only by __wrap_pthread_join() (see
// pthread_join_wrap.c), which must be linked directly into the target
// alongside `-Wl,--wrap=pthread_join`, so it can call __real_pthread_join().
int mc_pthread_join_maybe_defer(pthread_t, void**, bool *deferred);
int mc_sem_init(sem_t*, int, unsigned);
int mc_sem_post(sem_t*);
int mc_sem_wait(sem_t*);
int mc_sem_destroy(sem_t *sem);
unsigned mc_sleep(unsigned);
int mc_pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int mc_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int mc_pthread_cond_signal(pthread_cond_t *cond);
int mc_pthread_cond_broadcast(pthread_cond_t *cond);
int mc_pthread_cond_destroy(pthread_cond_t *cond);
void __mcmini_read(void *addr, size_t size, uintptr_t site_id);
void __mcmini_write(void *addr, size_t size, uintptr_t site_id);
int mc_pthread_barrier_init(pthread_barrier_t *barrier,
                            const pthread_barrierattr_t *attr,
                            unsigned count);
int mc_pthread_barrier_wait(pthread_barrier_t *barrier);
int mc_pthread_barrier_destroy(pthread_barrier_t *barrier);


/*
  An `atexit()` handler is installed in libmcmini.so with this function.
  This ensures that if the main thread exits the model checker still maintains
  control.
*/
void mc_exit_main_thread_in_child(void);
MCMINI_NO_RETURN void mc_transparent_abort(void);
MCMINI_NO_RETURN void mc_transparent_exit(int status);
MCMINI_NO_RETURN void mc_pthread_exit(void *retval);


/* TLS cache: per-thread direct-mapped cache of rec_list* pointers.
 * Eliminates pthread_rwlock on every hot-path interception. */
#define MCMINI_TLS_CACHE_BITS 8
#define MCMINI_TLS_CACHE_SIZE (1 << MCMINI_TLS_CACHE_BITS)

typedef struct {
    void     *addr;
    rec_list *rec;
} mc_tls_cache_entry;

extern __thread mc_tls_cache_entry mc_tls_obj_cache[MCMINI_TLS_CACHE_SIZE];

/*
 * This function implements the thread-safe "find-or-create" pattern
 * for any OBJECT record (mutex, cond, semaphore).
 *
 * It is static inline so each .c file gets its own high-performance
 * copy without linker errors.
 */
static inline rec_list* get_or_create_object_record(void *obj_addr,
                                                    visible_object_type obj_type,
                                                    int uninit_state)
{
    // rec_list *record;

    // // 1. Try with read lock
    // pthread_rwlock_rdlock(&rec_list_lock);
    // record = find_object_record_mode(obj_addr);
    // pthread_rwlock_unlock(&rec_list_lock);

    // if (record == NULL) {
    //     // 2. Need to create it, get write lock
    //     pthread_rwlock_wrlock(&rec_list_lock);

    //     // 3. MUST CHECK AGAIN (the "double-check")
    //     record = find_object_record_mode(obj_addr);
    //     if (record == NULL) {
    //         // 4. It's really not there. Create it.
    //         visible_object vo = {
    //             .type = obj_type,
    //             .location = obj_addr
    //         };
            
    //         // We have to set the correct union field for the initial state
    //         if (obj_type == MUTEX) {
    //             vo.mut_state = uninit_state;
    //         } else if (obj_type == SEMAPHORE) {
    //             vo.sem_state.status = uninit_state;
    //             vo.sem_state.count = 0; // Or some initial value
    //         } else if (obj_type == BARRIER) {
    //             vo.bar_state.status = uninit_state;
    //             vo.bar_state.count = 0;
    //             vo.bar_state.arrived = 0;
    //         } else if (obj_type == CONDITION_VARIABLE) {
    //             vo.cond_state.status = uninit_state;
    //             vo.cond_state.interacting_thread = 0;
    //             vo.cond_state.associated_mutex = NULL;
    //             vo.cond_state.count = 0;
    //             vo.cond_state.waiting_threads = create_thread_queue();
    //         }
            
    //         record = add_rec_entry_record_mode(&vo);
    //     }

    //     // 5. Release write lock
    //     pthread_rwlock_unlock(&rec_list_lock);
    // }

    /* TLS cache check: ~2 ns, no lock, no shared memory.
     * Empty slot has addr == NULL, safe since obj_addr is always non-NULL. */
    unsigned slot = ((uintptr_t)obj_addr >> 4) & (MCMINI_TLS_CACHE_SIZE - 1);
    mc_tls_cache_entry *e = &mc_tls_obj_cache[slot];
    if (__builtin_expect(e->addr == obj_addr, 1)) return e->rec;

    rec_list *record;

    /* Cache miss: pay rwlock once, then cache for this thread. */
    pthread_rwlock_rdlock(&rec_list_lock);
    record = find_object_record_mode(obj_addr);
    pthread_rwlock_unlock(&rec_list_lock);

    if (record == NULL) {
        pthread_rwlock_wrlock(&rec_list_lock);
        record = find_object_record_mode(obj_addr);
        if (record == NULL) {
            visible_object vo = {
                .type = obj_type,
                .location = obj_addr
            };
            if (obj_type == MUTEX) {
                vo.mut_state.status = uninit_state;
            } else if (obj_type == SEMAPHORE) {
                vo.sem_state.status = uninit_state;
                vo.sem_state.count = 0;
            } else if (obj_type == BARRIER) {
                vo.bar_state.status = uninit_state;
                vo.bar_state.count = 0;
                vo.bar_state.arrived = 0;
            } else if (obj_type == CONDITION_VARIABLE) {
                vo.cond_state.status = uninit_state;
                vo.cond_state.interacting_thread = 0;
                vo.cond_state.associated_mutex = NULL;
                vo.cond_state.count = 0;
                vo.cond_state.waiting_threads = create_thread_queue();
            }
            record = add_rec_entry_record_mode(&vo);
        }
        pthread_rwlock_unlock(&rec_list_lock);
    }

    /* Populate cache so next call from this thread skips the rwlock. */
    e->addr = obj_addr;
    e->rec  = record;
    return record;
}


/*
 * Thread-safe "find-or-create" for THREAD records.
 */
static inline rec_list* get_or_create_thread_record(pthread_t thrd_id,
                                                    runner_id_t runner_id, int status)
{
    rec_list *record;

    pthread_rwlock_rdlock(&rec_list_lock);
    record = find_thread_record_mode(thrd_id);
    pthread_rwlock_unlock(&rec_list_lock);

    if (record == NULL) {
        pthread_rwlock_wrlock(&rec_list_lock);
        // Double-check
        record = find_thread_record_mode(thrd_id);
        if (record == NULL) {
            visible_object vo = {.type = THREAD,
                               .location = NULL,
                               .thrd_state.pthread_desc = thrd_id,
                               .thrd_state.status = status,
                               .thrd_state.id = runner_id};
            record = add_rec_entry_record_mode(&vo);
        }
        pthread_rwlock_unlock(&rec_list_lock);
    }
    return record;
}
