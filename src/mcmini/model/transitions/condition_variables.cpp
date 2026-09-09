#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // for process_vm_readv(2)
#endif
#include <pthread.h>
#include <sys/uio.h>

#include <cstring>

#include "mcmini/mem.h"
#include "mcmini/model/exception.hpp"
#include "mcmini/model/transitions/condition_variables/callbacks.hpp"
#include "../include/mcmini/misc/cond/cond_var_arbitrary_policy.hpp"

using namespace model;
using namespace objects;

namespace {

/**
 * @brief Reads the bytes of the condition variable at `remote_cond` out of the
 * target process `target_pid` and reports whether they match the bit pattern
 * of a statically-initialized condition variable (`PTHREAD_COND_INITIALIZER`).
 *
 * @return true if the remote bytes equal `PTHREAD_COND_INITIALIZER`; false if
 * they differ or the remote memory could not be read.
 */
bool remote_cond_is_static_initializer(pid_t target_pid,
                                       pthread_cond_t *remote_cond) {
  if (target_pid <= 0) return false;
  pthread_cond_t initializer = PTHREAD_COND_INITIALIZER;
  pthread_cond_t local;
  struct iovec local_iov = {&local, sizeof(local)};
  struct iovec remote_iov = {(void *)remote_cond, sizeof(local)};
  ssize_t nread =
      process_vm_readv(target_pid, &local_iov, 1, &remote_iov, 1, 0);
  if (nread != (ssize_t)sizeof(local)) return false;
  return memcmp(&local, &initializer, sizeof(local)) == 0;
}

/**
 * @brief Ensures `remote_cond` is present in the model, supporting condition
 * variables declared with `PTHREAD_COND_INITIALIZER`.
 *
 * Such a condition variable never flows through the `pthread_cond_init`
 * wrapper, so the model never observes it being initialized. If the model has
 * not seen it, read the target's memory: if it matches
 * `PTHREAD_COND_INITIALIZER`, lazily register it as initialized (mirroring
 * `cond_init_callback`); otherwise report `ub_message` as undefined behavior.
 *
 * FIXME: As in the original McMini, this detects an uninitialized condition
 * variable only for stack-allocated (or otherwise garbage-filled) cvs. A
 * never-initialized cv in the data or heap segment is zero-filled, which on
 * glibc is identical to `PTHREAD_COND_INITIALIZER`, so it passes this check and
 * slips through undetected.
 */
void ensure_cond_initialized(model_to_system_map &m,
                             pthread_cond_t *remote_cond,
                             const char *ub_message) {
  if (m.contains(remote_cond)) return;
  if (!remote_cond_is_static_initializer(m.get_target_pid(), remote_cond))
    throw undefined_behavior_exception(ub_message);
  // FIXME: Allow dynamic selection of wakeup policies (mirrors
  // cond_init_callback).
  ConditionVariablePolicy *policy = new ConditionVariableArbitraryPolicy();
  m.observe_object(remote_cond, new condition_variable(
                                    condition_variable::state::cv_initialized,
                                    RID_INVALID, nullptr, 0, {}, policy));
}

}  // namespace

model::transition* cond_init_callback(runner_id_t p, 
                                      const volatile runner_mailbox& rmb,
                                      model_to_system_map& m) {
  // Fetch the remote object
  pthread_cond_t* remote_cond;
  memcpy_v(&remote_cond, (volatile void*)rmb.cnts, sizeof(pthread_cond_t*));

  // Locate the corresponding model of this object
  if (!m.contains(remote_cond)) {
    // FIXME: Allow dynamic selection of wakeup policies. For now, the
    // constructor's default (a fresh ConditionVariableArbitraryPolicy) is
    // hard-coded. Not great, but at least we can change it relatively
    // easily still.
    m.observe_object(remote_cond,
                     new condition_variable(condition_variable::state::cv_initialized));
  }
    
  state::objid_t const cond = m.get_model_of_object(remote_cond);
  return new transitions::condition_variable_init(p, cond);
}

model::transition* cond_waiting_thread_enqueue_callback(runner_id_t p,
                                                        const volatile runner_mailbox& rmb,
                                                        model_to_system_map& m){
  pthread_cond_t* remote_cond;
  pthread_mutex_t* remote_mut;
  memcpy_v(&remote_cond, (volatile void*)rmb.cnts, sizeof(pthread_cond_t*));
  memcpy_v(&remote_mut, (volatile void*)(rmb.cnts + sizeof(pthread_cond_t*)), sizeof(pthread_mutex_t*));

  ensure_cond_initialized(
      m, remote_cond,
      "Attempting to wait on an uninitialized condition variable");

  if (!m.contains(remote_mut))
    throw undefined_behavior_exception(
        "Attempting to wait on a condition variable with an uninitialized mutex");

  state::objid_t const cond = m.get_model_of_object(remote_cond);
  state::objid_t const mut = m.get_model_of_object(remote_mut);
  return new transitions::condition_variable_enqueue_thread(p, cond, mut);
}

model::transition* cond_wait_callback(runner_id_t p, 
                                      const volatile runner_mailbox& rmb,
                                      model_to_system_map& m) {
  pthread_cond_t* remote_cond;
  pthread_mutex_t* remote_mut;
  memcpy_v(&remote_cond, (volatile void*)rmb.cnts, sizeof(pthread_cond_t*));
  memcpy_v(&remote_mut, (volatile void*)(rmb.cnts + sizeof(pthread_cond_t*)), sizeof(pthread_mutex_t*));

  // Locate the corresponding model of this object
  if (!m.contains(remote_cond))
    throw undefined_behavior_exception(
        "Attempting to wait on an uninitialized condition variable");

  if (!m.contains(remote_mut))
    throw undefined_behavior_exception(
        "Attempting to wait on a condition variable with an uninitialized mutex");


  state::objid_t const cond = m.get_model_of_object(remote_cond);
  state::objid_t const mut = m.get_model_of_object(remote_mut);
  return new transitions::condition_variable_wait(p, cond, mut);
}

model::transition* cond_signal_callback(runner_id_t p, 
                                        const volatile runner_mailbox& rmb,
                                        model_to_system_map& m) {
  pthread_cond_t* remote_cond;
  memcpy_v(&remote_cond, (volatile void*)rmb.cnts, sizeof(pthread_cond_t*)); 

  // Locate the corresponding model of this object
  ensure_cond_initialized(
      m, remote_cond,
      "Attempting to signal an uninitialized condition variable");

  state::objid_t const cond = m.get_model_of_object(remote_cond);
  return new transitions::condition_variable_signal(p, cond);
}

model::transition* cond_broadcast_callback(runner_id_t p, 
                                           const volatile runner_mailbox& rmb,
                                           model_to_system_map& m) {
  pthread_cond_t* remote_cond;
  memcpy_v(&remote_cond, (volatile void*)rmb.cnts, sizeof(pthread_cond_t*)); 

  // Locate the corresponding model of this object
  ensure_cond_initialized(
      m, remote_cond,
      "Attempting to broadcast on an uninitialized condition variable");

  state::objid_t const cond = m.get_model_of_object(remote_cond);
  return new transitions::condition_variable_broadcast(p, cond);
}

model::transition* cond_destroy_callback(runner_id_t p, 
                                         const volatile runner_mailbox& rmb,
                                         model_to_system_map& m) {
  pthread_cond_t* remote_cond;
  memcpy_v(&remote_cond, (volatile void*)rmb.cnts, sizeof(pthread_cond_t*)); 

  // Locate the corresponding model of this object
  if (!m.contains(remote_cond))
  throw undefined_behavior_exception(
  "Attempting to destroy an uninitialized condition variable");

  state::objid_t const cond = m.get_model_of_object(remote_cond);
  return new transitions::condition_variable_destroy(p, cond);
}
