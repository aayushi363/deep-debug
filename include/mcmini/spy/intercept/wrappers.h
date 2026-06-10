#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "mcmini/lib/entry.h"
#include "mcmini/real_world/mailbox/runner_mailbox.h"
#include "mcmini/spy/checkpointing/record.h"
#include "mcmini/spy/checkpointing/rec_list.h"
#include "mcmini/spy/checkpointing/objects.h"

void thread_await_scheduler(void);
void thread_wake_scheduler_and_wait(void);
void thread_awake_scheduler_for_thread_finish_transition(void);
void thread_handle_after_dmtcp_restart(void);
volatile runner_mailbox *thread_get_mailbox(void);

int mc_pthread_mutex_init(pthread_mutex_t *mutex,
                          const pthread_mutexattr_t *mutexattr);
int mc_pthread_mutex_lock(pthread_mutex_t *mutex);
int mc_pthread_mutex_unlock(pthread_mutex_t *mutex);
int mc_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                    void *(*routine)(void *), void *arg);
int mc_sem_post(sem_t *);
int mc_sem_wait(sem_t *);
int mc_pthread_join(pthread_t, void**);
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


/*
  An `atexit()` handler is installed in libmcmini.so with this function.
  This ensures that if the main thread exits the model checker still maintains
  control.
*/
void mc_exit_main_thread_in_child(void);
MCMINI_NO_RETURN void mc_transparent_abort(void);
MCMINI_NO_RETURN void mc_transparent_exit(int status);

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
    rec_list *record;

    // 1. Try with read lock
    pthread_rwlock_rdlock(&rec_list_lock);
    record = find_object_record_mode(obj_addr);
    pthread_rwlock_unlock(&rec_list_lock);

    if (record == NULL) {
        // 2. Need to create it, get write lock
        pthread_rwlock_wrlock(&rec_list_lock);

        // 3. MUST CHECK AGAIN (the "double-check")
        record = find_object_record_mode(obj_addr);
        if (record == NULL) {
            // 4. It's really not there. Create it.
            visible_object vo = {
                .type = obj_type,
                .location = obj_addr
            };
            
            // We have to set the correct union field for the initial state
            if (obj_type == MUTEX) {
                vo.mut_state = uninit_state;
            } else if (obj_type == SEMAPHORE) {
                vo.sem_state.status = uninit_state;
                vo.sem_state.count = 0; // Or some initial value
            } else if (obj_type == CONDITION_VARIABLE) {
                vo.cond_state.status = uninit_state;
                vo.cond_state.interacting_thread = 0;
                vo.cond_state.associated_mutex = NULL;
                vo.cond_state.count = 0;
                vo.cond_state.waiting_threads = create_thread_queue();
            }
            
            record = add_rec_entry_record_mode(&vo);
        }

        // 5. Release write lock
        pthread_rwlock_unlock(&rec_list_lock);
    }

    // 6. Return the valid record
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
