#include <stddef.h>
#include <stdint.h>

#include "mcmini/mcmini.h"
#include "mcmini/spy/checkpointing/lockset.h"

typedef struct memory_access_payload {
  uintptr_t address;
  size_t size;
  uintptr_t site_id;
} memory_access_payload;

/// Phase-1 race-prediction gate, set true during RECORD when the lockset
/// predictor is enabled (see `set_current_mode`). Distinct from
/// `mcmini_mc_active`, which gates the Phase-2 scheduler path.
extern volatile atomic_bool mcmini_lockset_active;

/// Calling thread's runner id (defined in wrappers.c).
extern MCMINI_THREAD_LOCAL runner_id_t tid_self;

static MCMINI_THREAD_LOCAL int memory_hook_depth = 0;

void mcmini_reset_memory_hook_depth(void) { memory_hook_depth = 0; }

static void mcmini_memory_access(void *addr, size_t size, uintptr_t site_id,
                                 int is_write) {
  // Fast path (Caveat 1): the compiler instruments every load/store, so this
  // function is called extremely frequently. During the RECORD/checkpoint phase
  // the target must run at near-native speed. Two relaxed atomic loads
  // short-circuit before any thread-local access or the full mode switch:
  //   - `mcmini_mc_active`: the Phase-2 scheduler path (TARGET_BRANCH*/restart);
  //   - `mcmini_lockset_active`: the Phase-1 lockset predictor path (RECORD).
  const int mc_active = atomic_load_explicit(&mcmini_mc_active, memory_order_relaxed);
  const int ls_active =
      atomic_load_explicit(&mcmini_lockset_active, memory_order_relaxed);
  if (!mc_active && !ls_active) return;
  if (memory_hook_depth > 0) return;
  memory_hook_depth++;

  // Phase 1: feed the access into the cheap lockset predictor and return. No
  // scheduler/mailbox interaction; the target keeps running natively.
  if (!mc_active) {
    lockset_on_access(tid_self, addr, size, site_id, is_write);
    memory_hook_depth--;
    return;
  }

  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case CHECKPOINT_THREAD:
    case RECORD:
    case PRE_CHECKPOINT:
    case FUZZER_STANDALONE: {
      break;
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      memory_access_payload payload = {
          .address = (uintptr_t)addr, .size = size, .site_id = site_id};
      mb->type = is_write ? MEMORY_WRITE_TYPE : MEMORY_READ_TYPE;
      memcpy_v(mb->cnts, &payload, sizeof(payload));
      thread_handle_after_dmtcp_restart();
      break;
    }
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      volatile runner_mailbox *mb = thread_get_mailbox();
      memory_access_payload payload = {
          .address = (uintptr_t)addr, .size = size, .site_id = site_id};
      mb->type = is_write ? MEMORY_WRITE_TYPE : MEMORY_READ_TYPE;
      memcpy_v(mb->cnts, &payload, sizeof(payload));
      thread_wake_scheduler_and_wait();
      break;
    }
    default: {
      break;
    }
  }

  memory_hook_depth--;
}

void __mcmini_read(void *addr, size_t size, uintptr_t site_id) {
  mcmini_memory_access(addr, size, site_id, 0);
}

void __mcmini_write(void *addr, size_t size, uintptr_t site_id) {
  mcmini_memory_access(addr, size, site_id, 1);
}
