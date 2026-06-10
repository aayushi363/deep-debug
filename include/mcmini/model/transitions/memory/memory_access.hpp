#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mcmini/model/transition.hpp"

namespace model {
namespace transitions {

struct memory_access : public model::transition {
 public:
  enum class kind { read, write };

 private:
  const uintptr_t address;
  const size_t access_size;
  const uintptr_t site_id;
  const kind access_kind;

 public:
  memory_access(runner_id_t executor, uintptr_t address, size_t access_size,
                uintptr_t site_id, kind access_kind)
      : transition(executor),
        address(address),
        access_size(access_size),
        site_id(site_id),
        access_kind(access_kind) {}
  ~memory_access() = default;

  status modify(model::mutable_state&) const override {
    return status::exists;
  }

  uintptr_t get_address() const { return address; }
  size_t get_size() const { return access_size; }
  uintptr_t get_site_id() const { return site_id; }
  kind get_kind() const { return access_kind; }
  bool is_read() const { return access_kind == kind::read; }
  bool is_write() const { return access_kind == kind::write; }

  bool overlaps(const memory_access* other) const {
    if (access_size == 0 || other->access_size == 0) return false;
    const uintptr_t other_address = other->address;
    const uintptr_t this_end = address + access_size;
    const uintptr_t other_end = other_address + other->access_size;
    if (this_end < address || other_end < other_address) return true;
    return address < other_end && other_address < this_end;
  }

  bool depends(const memory_access* other) const {
    return overlaps(other) && (is_write() || other->is_write());
  }

  bool coenabled_with(const memory_access*) const { return true; }

  std::string to_string() const override {
    return std::string(is_write() ? "write" : "read") + "(addr:" +
           std::to_string(address) + ", size:" +
           std::to_string(access_size) + ", site:" +
           std::to_string(site_id) + ")";
  }
};

}  // namespace transitions
}  // namespace model
