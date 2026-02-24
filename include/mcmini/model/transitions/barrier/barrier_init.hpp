#pragma once

#include "mcmini/model/objects/barrier.hpp"
#include "mcmini/model/transition.hpp"

namespace model {
namespace transitions {

struct barrier_arrive;
struct barrier_pass;
struct barrier_destroy;

struct barrier_init : public model::transition {
 private:
  const state::objid_t barrier_id;
  const unsigned count;

 public:
  barrier_init(runner_id_t executor, state::objid_t barrier_id, unsigned count)
      : transition(executor), barrier_id(barrier_id), count(count) {}
  ~barrier_init() = default;
  
  status modify(model::mutable_state& s) const override {
    using namespace model::objects;
    s.add_state_for_obj(barrier_id, new barrier(barrier::initialized, count));
    return status::exists;
  }
  
  state::objid_t get_id() const { return this->barrier_id; }
  
  std::string to_string() const override {
    return "barrier_init(barrier:" + std::to_string(barrier_id) + 
           ", count:" + std::to_string(count) + ")";
  }

  // MARK: Model checking functions (defined in .cpp)
  bool depends(const barrier_init* bi) const;
  bool depends(const barrier_arrive* ba) const;
  bool depends(const barrier_pass* bp) const;
  bool depends(const barrier_destroy* bd) const;
};

}  // namespace transitions
}  // namespace model
