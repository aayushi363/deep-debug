#include "mcmini/mem.h"
#include "mcmini/spy/checkpointing/record.h"
#include "mcmini/spy/checkpointing/rec_list.h"
#include "mcmini/spy/checkpointing/objects.h"
#include "mcmini/spy/checkpointing/transitions.h"
#include "mcmini/spy/checkpointing/tsan_support.h"
#include "mcmini/spy/intercept/interception.h"
#include "mcmini/spy/checkpointing/uthash.h"
#include "mcmini/spy/checkpointing/lockset.h"
#include "mcmini/wrapper_timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

sem_t dmtcp_restart_sem;
// Previously we used a mutex here; the codebase now uses a read-write lock.
// Provide the actual definition (not extern) so the symbol is available to
// other translation units and at dynamic link time.
pthread_rwlock_t rec_list_lock = PTHREAD_RWLOCK_INITIALIZER;
pthread_mutex_t pending_op_lock = PTHREAD_MUTEX_INITIALIZER;
volatile atomic_int libmcmini_mode = PRE_DMTCP_INIT;
volatile atomic_bool mcmini_mc_active = false;
volatile atomic_bool mcmini_lockset_active = false;
visible_object empty_visible_obj = {.type = UNKNOWN, .location = NULL};
rec_list *head_record_mode = NULL;
rec_list *current_record_mode = NULL;
// head for hash table (for fast lookup)
rec_list *object_hash_map = NULL;

transition invisible_operation_for_this_thread(void) {
  transition t = {.type = INVISIBLE_OPERATION_TYPE, .executor = pthread_self()};
  return t;
}

rec_list *find_object(void *addr, rec_list *head) {
  for (rec_list *node = head; node != NULL; node = node->next) {
    if (node->vo.location == addr) return node;
  }
  return NULL;
}

rec_list *find_thread_record_mode(pthread_t thrd) {
  for (rec_list *node = head_record_mode; node != NULL; node = node->next) {
    if (node->vo.type == THREAD &&
        pthread_equal(thrd, node->vo.thrd_state.pthread_desc))
      return node;
  }
  return NULL;
}

// This is O(n); replaced by hash map version
// rec_list *find_object_record_mode(void *addr) {
//   return find_object(addr, head_record_mode);
// }

// Hash map version of find_object_record_mode
// with O(1) average time complexity
rec_list *find_object_record_mode(void *addr){
  MEASURE_FUNCTION_TIME
  rec_list *node;
  HASH_FIND_PTR(object_hash_map, &addr, node);
  return node;
}

rec_list *add_rec_entry(const visible_object *vo, rec_list **head, rec_list **current) {
  rec_list *new_node = (rec_list *)malloc(sizeof(rec_list));
  if (new_node == NULL) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  new_node->vo = *vo;
  libpthread_mutex_init(&new_node->node_lock, NULL);
  libpthread_cond_init(&new_node->node_cond, NULL);
  if (*head == NULL) {
    *head = new_node;
    *current = new_node;
  }
  else {
    (*current)->next = new_node;
    *current = new_node;
  }
  return new_node;
}

// rec_list *add_rec_entry_record_mode(const visible_object *vo) {
//   rec_list *new_node = (rec_list *)malloc(sizeof(rec_list));
//   if (new_node == NULL) {
//     perror("malloc");
//     exit(EXIT_FAILURE);
//   }
//   new_node->vo = *vo;
//   new_node->next = NULL;
//   if (head_record_mode == NULL) {
//     head_record_mode = new_node;
//     current_record_mode = new_node;
//   }
//   else {
//     current_record_mode->next = new_node;
//     current_record_mode = new_node;
//   }
//   return new_node;
// }

rec_list *add_rec_entry_record_mode(const visible_object *vo) {
  MEASURE_FUNCTION_TIME
  rec_list *new_node = (rec_list *)malloc(sizeof(rec_list));
  if (new_node == NULL) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  new_node->vo = *vo;
  new_node->next = NULL;

  libpthread_mutex_init(&new_node->node_lock, NULL);
  libpthread_cond_init(&new_node->node_cond, NULL);

  // Must clear the hash handle before adding
  memset(&new_node->hh, 0, sizeof(new_node->hh));

  // 1. Add to the linked list (SAME AS BEFORE)
  // This ensures the snapshotter sees it in the correct order.
  if (head_record_mode == NULL) {
    head_record_mode = new_node;
    current_record_mode = new_node;
  }
  else {
    current_record_mode->next = new_node;
    current_record_mode = new_node;
  }

  // 2. Add to the hash table (THE NEW PART)
  // This ensures future lookups are O(1).
  // We use vo->location as the key.
  HASH_ADD_PTR(object_hash_map, vo.location, new_node);

  return new_node;
}

// TSan-safe variant: identical to add_rec_entry_record_mode but backed by
// mc_ts_alloc (no malloc), for use before libtsan has registered the calling
// thread. `new_node->vo = *vo` compiles to inline stores (verified: no memcpy
// libcall), so this whole function stays free of TSan interceptors.
rec_list *add_rec_entry_record_mode_ts(const visible_object *vo) {
  rec_list *new_node = (rec_list *)mc_ts_alloc(sizeof(rec_list));
  new_node->vo = *vo;
  new_node->next = NULL;
  if (head_record_mode == NULL) {
    head_record_mode = new_node;
    current_record_mode = new_node;
  } else {
    current_record_mode->next = new_node;
    current_record_mode = new_node;
  }
  return new_node;
}

//debugging puropses, will remove later
// void print_rec_list(const rec_list *head) {
//   const rec_list *current = head;
//   while (current != NULL) {
//     const visible_object *vo = &current->vo;

//     printf("Record:\n");
//     printf("  Type: ");
//     switch (vo->type) {
//       case UNKNOWN:
//         printf("UNKNOWN\n");
//         break;
//       case MUTEX:
//         printf("MUTEX\n");
//         printf("  Location: %p\n", vo->location);
//         printf("  Mutex State: ");
//         switch (vo->mut_state) {
//           case UNINITIALIZED: printf("UNINITIALIZED\n"); break;
//           case UNLOCKED: printf("UNLOCKED\n"); break;
//           case LOCKED: printf("LOCKED\n"); break;
//           case DESTROYED: printf("DESTROYED\n"); break;
//           default: printf("UNKNOWN STATE\n"); break;
//         }
//         break;
//       case SEMAPHORE:
//         printf("SEMAPHORE\n");
//         printf("  Location: %p\n", vo->location);
//         printf("  Count: %d\n", vo->sem_state.count);
//         break;
//       case CONDITION_VARIABLE:
//         printf("CONDITION VARIABLE\n");
//         printf("  Location: %p\n", vo->location);
//         printf("  Status: ");
//         switch (vo->cond_state.status) {
//           case CV_UNINITIALIZED: printf("UNINITIALIZED\n"); break;
//           case CV_INITIALIZED: printf("INITIALIZED\n"); break;
//           case CV_WAITING: printf("WAITING\n"); break;
//           case CV_SIGNALED: printf("SIGNALED\n"); break;
//           case CV_PREWAITING: printf("TRANSITIONAL\n"); break;
//           default: printf("UNKNOWN STATUS\n"); break;
//         }
//         printf("  Waiting Thread: %p\n", (void *)vo->cond_state.interacting_thread);
//         printf("  Associated Mutex: %p\n", (void *)vo->cond_state.associated_mutex);
//         printf("  Waiting Count: %d\n", vo->cond_state.count);
//         break;
//       case THREAD:
//         printf("THREAD\n");
//         printf("  Thread Descriptor: %p\n", (void *)vo->thrd_state.pthread_desc);
//         printf("  Runner ID: %u\n", vo->thrd_state.id);
//         printf("  Status: ");
//         switch (vo->thrd_state.status) {
//           case ALIVE: printf("ALIVE\n"); break;
//           case EXITED: printf("EXITED\n"); break;
//           default: printf("UNKNOWN STATUS\n"); break;
//         }
//         break;
//       default:
//         printf("INVALID TYPE\n");
//         break;
//     }

//     current = current->next;
//   }
// }

void notify_template_thread() { libpthread_sem_post(&dmtcp_restart_sem); }

bool is_in_restart_mode(void) {
  enum libmcmini_mode mode = get_current_mode();
  return mode == DMTCP_RESTART_INTO_BRANCH ||
         mode == DMTCP_RESTART_INTO_TEMPLATE;
}

enum libmcmini_mode get_current_mode() {
  resolve_ckpt_window_candidate_if_pending();

  // INVARIANT (imposed by the code in `mc_pthread_create()`):
  // The checkpoint thread is guaranteed to reach the line
  // "is_checkpoint_thread()", because the only time in which the atomic is
  // stored is BEFORE the checkpoint thread has executed DMTCP code.
  if (atomic_load(&libmcmini_has_recorded_checkpoint_thread)) {
    if (is_checkpoint_thread()) {
      return EXTERNAL_THREAD;
    }
  }
  enum libmcmini_mode raw_mode = atomic_load(&libmcmini_mode);
  // mc_is_current_thread_tsan_internal() is only ever needed to distinguish
  // TSan's internal thread in the four modes below (the same four every
  // wrapper function's own switch groups together for this purpose) --
  // never during PRE_DMTCP_INIT, PRE_CHECKPOINT_THREAD, RECORD, or
  // PRE_CHECKPOINT. Its first call per thread leaks an fd (see
  // tsan_support.c), and calling it outside these four modes means that
  // leaked fd is open at checkpoint time, so DMTCP must checkpoint/restore
  // a /proc/<pid>/task/<tid>/status path whose pid/tid cannot exist after
  // restart -- template_thread() then hangs waiting for a
  // restart-completion signal that can never arrive.
  bool tsan_internal_check_applies =
      raw_mode == TARGET_BRANCH || raw_mode == TARGET_BRANCH_AFTER_RESTART ||
      raw_mode == DMTCP_RESTART_INTO_BRANCH ||
      raw_mode == DMTCP_RESTART_INTO_TEMPLATE;
  if (!tsan_internal_check_applies) {
    return raw_mode;
  }
  // ThreadSanitizer's own internal background thread (present only when the
  // target is TSan-instrumented) can likewise call into libmcmini.so's
  // overridden functions for its own purposes, on a thread that is not part
  // of the target program. See EXTERNAL_THREAD's definition in record.h.
  if (mc_is_current_thread_tsan_internal()) {
    return EXTERNAL_THREAD;
  }
  return raw_mode;
}
void set_current_mode(enum libmcmini_mode new_mode) {
  // Store the mode first, then derive the memory-hook fast-path gate, so that a
  // thread observing `mcmini_mc_active == true` always reads a correct, active
  // mode in the hook's full switch. The gate is `true` only in the phases where
  // instrumented memory accesses are live model-checking transitions.
  atomic_store(&libmcmini_mode, new_mode);
  const bool active =
      new_mode == DMTCP_RESTART_INTO_BRANCH ||
      new_mode == DMTCP_RESTART_INTO_TEMPLATE || new_mode == TARGET_BRANCH ||
      new_mode == TARGET_BRANCH_AFTER_RESTART;
  atomic_store(&mcmini_mc_active, active);

  // Enable the Phase-1 lockset predictor while the target runs natively during
  // recording (and right up to the checkpoint). Only when `MCMINI_LOCKSET` is
  // set; otherwise this stays false and the memory hooks behave exactly as
  // before. Must be mutually exclusive with `mcmini_mc_active` so the hook
  // takes the cheap predictor path, not the scheduler path.
  const bool lockset_active =
      !active && lockset_is_enabled() &&
      (new_mode == RECORD || new_mode == PRE_CHECKPOINT);
  atomic_store(&mcmini_lockset_active, lockset_active);
}
