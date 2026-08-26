#pragma once

#include "mcmini/spy/checkpointing/objects.h"
#include "mcmini/spy/checkpointing/uthash.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* volatile unsigned char works as a spinlock in both C and C++ without
 * pulling in <stdatomic.h> (which is C-only and breaks C++ TUs). */
typedef volatile unsigned char mc_spinlock_t;

typedef struct rec_list {
  visible_object vo;
  pthread_mutex_t node_lock; /* barrier only: required by pthread_cond_timedwait */
  mc_spinlock_t   node_spin; /* mutex/condvar/sem hot path: userspace spinlock */
  pthread_cond_t  node_cond;
  struct rec_list *next;
  UT_hash_handle hh;
} rec_list;

static inline void node_spin_lock(mc_spinlock_t *f) {
  while (__atomic_test_and_set(f, __ATOMIC_ACQUIRE));
}
static inline void node_spin_unlock(mc_spinlock_t *f) {
  __atomic_clear(f, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
} // extern "C"
#endif
