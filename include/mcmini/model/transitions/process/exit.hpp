#pragma once

#include "mcmini/model/transition.hpp"

namespace model {
namespace transitions {

struct process_exit : public model::transition {
 public:
  int exit_code = -1;
  process_exit(state::runner_id_t executor) : process_exit(executor, -1) {}
  process_exit(state::runner_id_t executor, int exit_code)
      : transition(executor), exit_code(exit_code) {}
  ~process_exit() = default;

  status modify(model::mutable_state& s) const override {
    // We ensure that exiting is never enabled. This ensures that it will never
    // be explored by any model checking algorithm
    return status::exists;
  }

  int program_exit_code() const override { return exit_code; }

  std::string to_string() const override { return "exit(2) (syscall)"; }
  std::string to_json() const override {
    return "{" + json_header() + ",\"op\":\"process_exit\",\"exit_code\":" + std::to_string(exit_code) + "}";
  }
};

}  // namespace transitions
}  // namespace model
