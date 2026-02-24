#pragma once

#include "mcmini/model/objects/barrier.hpp"
#include "mcmini/model/transition.hpp"

namespace model {
namespace transitions {

struct barrier_init;
struct barrier_arrive;
struct barrier_pass;

struct barrier_destroy : public model::transition {
 private:
  const state::objid_t barrier_id;

 public:
  barrier_destroy(runner_id_t executor, state::objid_t barrier_id)
      : transition(executor), barrier_id(barrier_id) {}
  ~barrier_destroy() = default;
  
  status modify(model::mutable_state& s) const override {
    using namespace model::objects;
    const barrier* bar = s.get_state_of_object<barrier>(barrier_id);
    barrier* destroyed = new barrier(*bar);
    destroyed->destroy();
    s.add_state_for_obj(barrier_id, destroyed);
    return status::exists;
  }
  
  state::objid_t get_id() const { return this->barrier_id; }
  
  std::string to_string() const override {
    return "barrier_destroy(barrier:" + std::to_string(barrier_id) + ")";
  }

  // MARK: Model checking functions (defined in .cpp)
  bool depends(const barrier_init* bi) const;
  bool depends(const barrier_arrive* ba) const;
  bool depends(const barrier_pass* bp) const;
  bool depends(const barrier_destroy* bd) const;
};

}  // namespace transitions
}  // namespace model
