#pragma once

#include <set>
#include <string>
#include <unordered_map>

#include "mcmini/misc/extensions/unique_ptr.hpp"
#include "mcmini/model/visible_object_state.hpp"
#include "mcmini/forwards.hpp"

namespace model {
namespace objects {
struct barrier : public model::visible_object_state {
 public:
  enum state { uninitialized, initialized, destroyed };

 private:
  unsigned _count;    // Number of threads required (waitCount)
  std::set<runner_id_t> _threads_waiting_odd;   // Threads in odd generation
  std::set<runner_id_t> _threads_waiting_even;  // Threads in even generation
  bool _is_even;      // Current parity/generation
  unsigned _generation = 0;  // Monotonic counter: incremented each time barrier fires
  // Records the generation at which each thread called wait().  Used by the
  // barrier_pass callback to reconstruct the same firing-cycle generation that
  // the corresponding barrier_arrive transition captured.
  std::unordered_map<runner_id_t, unsigned> _arrive_gen;
  state current_state = state::uninitialized;

 public:
  barrier() = default;
  ~barrier() = default;
  barrier(const barrier &) = default;
  explicit barrier(state s) : barrier(s, 0) {}
  explicit barrier(unsigned count) : barrier(initialized, count) {}
  explicit barrier(state s, unsigned count)
      : _count(count), _is_even(false), current_state(s) {}

  // Get size of current waiting set
  unsigned current_waiting_count() const {
    auto& current_set = _is_even ? _threads_waiting_even : _threads_waiting_odd;
    return current_set.size();
  }

  // Add thread to current generation's waiting set.
  // Idempotent: a thread already in any waiting set is not re-inserted.
  // Records the generation at arrival time so the pass callback can retrieve it.
  void wait(runner_id_t tid) {
    if (has_thread(tid)) return;
    // Record the generation BEFORE any flip happens.
    _arrive_gen[tid] = _generation;
    auto& current_set = _is_even ? _threads_waiting_even : _threads_waiting_odd;
    current_set.insert(tid);

    // If this completes the barrier, flip parity and advance generation counter
    if (current_set.size() >= _count) {
      _is_even = !_is_even;
      _generation++;
    }
  }

  // Remove thread from both waiting sets.  Called after a thread passes
  // through the barrier so that the barrier can be correctly reused.
  void leave(runner_id_t tid) {
    _threads_waiting_even.erase(tid);
    _threads_waiting_odd.erase(tid);
    _arrive_gen.erase(tid);
  }

  // Check if thread would block if it waited
  bool would_block_if_waited_on(runner_id_t tid) const {
    // Determine which set this thread is in
    bool thread_in_even = _threads_waiting_even.find(tid) != _threads_waiting_even.end();
    bool thread_in_odd = _threads_waiting_odd.find(tid) != _threads_waiting_odd.end();

    if (!thread_in_even && !thread_in_odd) {
      // Thread hasn't arrived yet - would need to wait
      return true;
    }

    // Thread has arrived - check if its generation matches current barrier parity
    bool thread_parity = thread_in_even;
    if (thread_parity == _is_even) {
      // Same parity means thread is waiting in current generation
      auto& current_set = _is_even ? _threads_waiting_even : _threads_waiting_odd;
      return current_set.size() < _count;
    }

    // Different parity means barrier was satisfied, thread can proceed
    return false;
  }

  bool has_thread(runner_id_t tid) const {
    return (_threads_waiting_odd.find(tid) != _threads_waiting_odd.end()) ||
           (_threads_waiting_even.find(tid) != _threads_waiting_even.end());
  }

  // Returns the generation at which tid called wait() (i.e. its arrive-cycle
  // generation).  Used by barrier_pass_callback to tag the pass transition with
  // the same generation as the corresponding barrier_arrive transition.
  unsigned get_arrive_gen(runner_id_t tid) const {
    auto it = _arrive_gen.find(tid);
    if (it == _arrive_gen.end()) return _generation;  // fallback: current gen
    return it->second;
  }

  void destroy() { this->current_state = state::destroyed; }

  unsigned count() const { return this->_count; }
  unsigned generation() const { return this->_generation; }
  unsigned arrived() const {
    auto& current_set = _is_even ? _threads_waiting_even : _threads_waiting_odd;
    return current_set.size();
  }
  bool is_full() const { return arrived() >= _count; }

  std::unique_ptr<visible_object_state> clone() const override {
    return extensions::make_unique<barrier>(*this);
  }

  std::string to_string() const override {
    return "barrier(count: " + std::to_string(_count) +
           ", arrived: " + std::to_string(arrived()) +
           ", parity: " + (_is_even ? "even" : "odd") + ")";
  }
};
}  // namespace objects
}  // namespace model
