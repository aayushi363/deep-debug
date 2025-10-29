#pragma once

#include "mcmini/spy/checkpointing/objects.h"
#include "mcmini/spy/checkpointing/uthash.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rec_list {
  visible_object vo;
  struct rec_list *next;
  UT_hash_handle hh; // makes this structure hashable
} rec_list;


#ifdef __cplusplus
} // extern "C"
#endif
