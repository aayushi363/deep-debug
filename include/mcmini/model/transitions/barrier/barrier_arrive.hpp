#pragma once

#include "mcmini/model/objects/barrier.hpp"
#include "mcmini/model/transition.hpp"

namespace model {
namespace transitions {

struct barrier_pass;
struct barrier_destroy;
struct mutex_lock;
struct mutex_unlock;

// Phase 1: Thread arrives at barrier (always enabled)
struct barrier_arrive : public model::transition {
 private:
  const state::objid_t barrier_id;
  const unsigned _generation;  // Barrier generation at time of arrival

 public:
  barrier_arrive(runner_id_t executor, state::objid_t barrier_id,
                 unsigned generation)
      : transition(executor), barrier_id(barrier_id), _generation(generation) {}
  ~barrier_arrive() = default;

  status modify(model::mutable_state& s) const override {
    using namespace model::objects;
    const barrier* bar = s.get_state_of_object<barrier>(barrier_id);

    // Always enabled - record the thread's arrival
    barrier* new_bar = new barrier(*bar);
    new_bar->wait(executor);  // Adds to current parity set, may flip parity
    s.add_state_for_obj(barrier_id, new_bar);

    // Always succeeds (enabled)
    return status::exists;
  }

  state::objid_t get_id() const { return this->barrier_id; }
  unsigned get_generation() const { return this->_generation; }

  std::string to_string() const override {
    return "barrier_arrive(barrier:" + std::to_string(barrier_id) +
           ", gen:" + std::to_string(_generation) + ")";
  }

  // MARK: Model checking functions (defined in .cpp)
  bool depends(const barrier_arrive* ba) const;
  bool depends(const barrier_pass* bp) const;
  bool depends(const barrier_destroy* bd) const;
  bool coenabled_with(const barrier_arrive*) const;
  bool coenabled_with(const barrier_pass* bp) const;

  // Barriers and mutexes operate on entirely different objects: always independent
  bool depends(const mutex_lock*) const { return false; }
  bool depends(const mutex_unlock*) const { return false; }
};

}  // namespace transitions
}  // namespace model
