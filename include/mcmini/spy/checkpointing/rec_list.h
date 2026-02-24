#pragma once

#include "mcmini/spy/checkpointing/objects.h"
#include "mcmini/spy/checkpointing/uthash.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rec_list {
  visible_object vo;
  pthread_mutex_t node_lock; // Lock for this node
  pthread_cond_t  node_cond; // Condition variable for timed barrier waiting
  struct rec_list *next;
  UT_hash_handle hh; // makes this structure hashable
} rec_list;


#ifdef __cplusplus
} // extern "C"
#endif
