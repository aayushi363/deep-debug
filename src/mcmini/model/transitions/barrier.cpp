#include "mcmini/mem.h"
#include "mcmini/model/exception.hpp"
#include "mcmini/model/transitions/barrier/callbacks.hpp"
#include "mcmini/model/transitions/barrier/barrier_init.hpp"
#include "mcmini/model/transitions/barrier/barrier_arrive.hpp"
#include "mcmini/model/transitions/barrier/barrier_wait.hpp"
#include "mcmini/model/transitions/barrier/barrier_destroy.hpp"

using namespace model;
using namespace objects;

model::transition* barrier_init_callback(runner_id_t p,
                                         const volatile runner_mailbox& rmb,
                                         model_to_system_map& m) {
  // Fetch the remote object
  unsigned count;
  pthread_barrier_t* remote_barrier;
  memcpy_v(&remote_barrier, (volatile void*)rmb.cnts, sizeof(pthread_barrier_t*));
  memcpy_v(&count, (volatile void*)(rmb.cnts + sizeof(pthread_barrier_t*)), sizeof(unsigned));

  // Locate the corresponding model of this object
  if (!m.contains(remote_barrier)) m.observe_object(remote_barrier, new barrier());

  const state::objid_t model_barrier = m.get_model_of_object(remote_barrier);
  return new transitions::barrier_init(p, model_barrier, count);
}

model::transition* barrier_arrive_callback(runner_id_t p,
                                           const volatile runner_mailbox& rmb,
                                           model_to_system_map& m) {
  pthread_barrier_t* remote_barrier;
  memcpy_v(&remote_barrier, (volatile void*)rmb.cnts, sizeof(pthread_barrier_t*));

  if (!m.contains(remote_barrier))
    throw undefined_behavior_exception(
        "Attempting to wait on an uninitialized barrier");

  const state::objid_t model_barrier = m.get_model_of_object(remote_barrier);

  // Capture the barrier's current generation so DPOR can distinguish which
  // arrive/pass pairs belong to the same firing cycle.
  const auto* bar = static_cast<const barrier*>(
      m.get_current_object_state(model_barrier));
  unsigned gen = bar->generation();

  return new transitions::barrier_arrive(p, model_barrier, gen);
}

model::transition* barrier_pass_callback(runner_id_t p,
                                         const volatile runner_mailbox& rmb,
                                         model_to_system_map& m) {
  pthread_barrier_t* remote_barrier;
  memcpy_v(&remote_barrier, (volatile void*)rmb.cnts, sizeof(pthread_barrier_t*));

  if (!m.contains(remote_barrier))
    throw undefined_behavior_exception(
        "Attempting to wait on an uninitialized barrier");

  const state::objid_t model_barrier = m.get_model_of_object(remote_barrier);

  // Look up the arrive-cycle generation for this thread.  barrier_arrive::
  // modify() already called bar->wait(p) which recorded _arrive_gen[p] =
  // generation-before-flip.  Using get_arrive_gen() here ensures that the
  // generation stored in the pass transition ALWAYS matches the generation
  // stored in the corresponding arrive transition for the same firing cycle,
  // regardless of whether the barrier fired before or after this callback.
  const auto* bar = static_cast<const barrier*>(
      m.get_current_object_state(model_barrier));
  unsigned gen = bar->get_arrive_gen(p);

  // barrier_arrive already added this thread to the barrier's waiting set
  // (two-phase protocol: ARRIVE is always executed before PASS for the same
  // pthread_barrier_wait() call).  Do not modify barrier state here; just
  // create the transition and let barrier_pass::modify() handle the
  // enablement check and cleanup.
  return new transitions::barrier_pass(p, model_barrier, gen);
}

model::transition* barrier_destroy_callback(runner_id_t p,
                                            const volatile runner_mailbox& rmb,
                                            model_to_system_map& m) {
  pthread_barrier_t* remote_barrier;
  memcpy_v(&remote_barrier, (volatile void*)rmb.cnts, sizeof(pthread_barrier_t*));

  if (!m.contains(remote_barrier))
    throw undefined_behavior_exception(
        "Attempting to destroy an uninitialized barrier");

  const state::objid_t model_barrier = m.get_model_of_object(remote_barrier);
  return new transitions::barrier_destroy(p, model_barrier);
}

// MARK: Dependency and Co-enablement Relations

// barrier_init dependency methods
namespace model {
namespace transitions {

bool barrier_init::depends(const barrier_init* bi) const {
  return this->barrier_id == bi->get_id();
}

bool barrier_init::depends(const barrier_arrive* ba) const {
  return this->barrier_id == ba->get_id();
}

bool barrier_init::depends(const barrier_pass* bp) const {
  return this->barrier_id == bp->get_id();
}

bool barrier_init::depends(const barrier_destroy* bd) const {
  return this->barrier_id == bd->get_id();
}

// barrier_arrive dependency and co-enablement methods
bool barrier_arrive::depends(const barrier_arrive* ba) const {
  // Arrivals are commutative - T1_arrive; T2_arrive produces same state
  // as T2_arrive; T1_arrive. Order doesn't matter, so they're independent.
  return false;
}

bool barrier_arrive::depends(const barrier_pass* bp) const {
  // Conservative: any arrive/pass on the SAME barrier are dependent.
  //
  // We deliberately do NOT refine this by generation. A transition's stored
  // _generation is a snapshot taken in the discovery callback, but the cycle a
  // thread actually arrives in is interleaving-dependent whenever more than
  // `count` threads use a barrier (whoever is modeled as the (count+1)-th
  // arrival lands in the next generation). No fixed snapshot can be correct
  // for every interleaving DPOR reorders, so a generation-equality test could
  // declare a same-thread arrive->pass pair "independent" and drop a required
  // backtracking point (unsound reduction). Depending on barrier_id alone
  // over-approximates dependencies, which is always sound for DPOR.
  return this->barrier_id == bp->get_id();
}

bool barrier_arrive::depends(const barrier_destroy* bd) const {
  return this->barrier_id == bd->get_id();
}

bool barrier_arrive::coenabled_with(const barrier_arrive*) const {
  // Arrivals are always co-enabled (always allowed to arrive)
  return true;
}

bool barrier_arrive::coenabled_with(const barrier_pass* bp) const {
  // Conservative: assume arrive and pass can be co-enabled. When more than
  // `count` threads use a barrier, a pass (barrier already satisfied) and a
  // straggler's arrive (always enabled) genuinely are enabled in the same
  // state, so we cannot rule co-enablement out. Reporting "not co-enabled"
  // when they actually are could cause DPOR to skip a needed backtracking
  // point; reporting co-enabled is the safe over-approximation. As with
  // depends(), we do not refine by generation (see barrier_arrive::depends).
  return true;
}

// barrier_pass dependency and co-enablement methods
bool barrier_pass::depends(const barrier_arrive* ba) const {
  // Symmetric to barrier_arrive::depends(barrier_pass*): conservative, by
  // barrier_id only (no generation refinement — see the note there).
  return this->barrier_id == ba->get_id();
}

bool barrier_pass::depends(const barrier_pass* bp) const {
  // Passes are commutative - once barrier satisfied, threads can
  // pass in any order without affecting outcome
  return false;
}

bool barrier_pass::depends(const barrier_destroy* bd) const {
  return this->barrier_id == bd->get_id();
}

bool barrier_pass::coenabled_with(const barrier_pass* bp) const {
  // Once the barrier is satisfied all passes are co-enabled (any order).
  return true;
}

bool barrier_pass::coenabled_with(const barrier_arrive* ba) const {
  // Symmetric to barrier_arrive::coenabled_with(barrier_pass*): conservative.
  return true;
}

// barrier_destroy dependency methods
bool barrier_destroy::depends(const barrier_init* bi) const {
  return this->barrier_id == bi->get_id();
}

bool barrier_destroy::depends(const barrier_arrive* ba) const {
  return this->barrier_id == ba->get_id();
}

bool barrier_destroy::depends(const barrier_pass* bp) const {
  return this->barrier_id == bp->get_id();
}

bool barrier_destroy::depends(const barrier_destroy* bd) const {
  return this->barrier_id == bd->get_id();
}

}  // namespace transitions
}  // namespace model
