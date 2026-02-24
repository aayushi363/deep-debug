#pragma once

#include "mcmini/model/objects/barrier.hpp"
#include "mcmini/model/transition.hpp"

namespace model {
namespace transitions {

struct barrier_arrive;
struct barrier_destroy;
struct mutex_lock;
struct mutex_unlock;

// Phase 2: Thread passes through barrier (enabled only when barrier satisfied)
struct barrier_pass : public model::transition {
 private:
  const state::objid_t barrier_id;
  const unsigned _generation;  // Barrier generation at time of pass

 public:
  barrier_pass(runner_id_t executor, state::objid_t barrier_id,
               unsigned generation)
      : transition(executor), barrier_id(barrier_id), _generation(generation) {}
  ~barrier_pass() = default;

  status modify(model::mutable_state& s) const override {
    using namespace model::objects;
    const barrier* bar = s.get_state_of_object<barrier>(barrier_id);

    // barrier_arrive already ran for this thread (two-phase protocol), so the
    // thread is in the waiting set.  If the barrier has not yet fired (not
    // enough arrivals), this transition is disabled and the scheduler will
    // defer it until all N barrier_arrive transitions have executed.
    if (bar->would_block_if_waited_on(executor)) {
      return status::disabled;
    }

    // Barrier is satisfied.  Remove this thread from the waiting sets so that
    // the barrier object is left in a clean state for subsequent reuse.
    barrier* new_bar = new barrier(*bar);
    new_bar->leave(executor);
    s.add_state_for_obj(barrier_id, new_bar);
    return status::exists;
  }

  state::objid_t get_id() const { return this->barrier_id; }
  unsigned get_generation() const { return this->_generation; }

  std::string to_string() const override {
    return "barrier_pass(barrier:" + std::to_string(barrier_id) +
           ", gen:" + std::to_string(_generation) + ")";
  }

  // MARK: Model checking functions (defined in .cpp)
  bool depends(const barrier_arrive* ba) const;
  bool depends(const barrier_pass* bp) const;
  bool depends(const barrier_destroy* bd) const;
  bool coenabled_with(const barrier_pass* bp) const;
  bool coenabled_with(const barrier_arrive* ba) const;

  // Barriers and mutexes operate on entirely different objects: always independent
  bool depends(const mutex_lock*) const { return false; }
  bool depends(const mutex_unlock*) const { return false; }
};

}  // namespace transitions
}  // namespace model
