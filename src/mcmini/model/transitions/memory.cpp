#include "mcmini/model/transitions/memory/callbacks.hpp"

#include <stdint.h>

#include "mcmini/mem.h"

using namespace model;

struct memory_access_payload {
  uintptr_t address;
  size_t size;
  uintptr_t site_id;
};

static memory_access_payload read_memory_access_payload(
    const volatile runner_mailbox& rmb) {
  memory_access_payload payload;
  memcpy_v(&payload, (volatile void*)rmb.cnts, sizeof(payload));
  return payload;
}

model::transition* memory_read_callback(runner_id_t p,
                                        const volatile runner_mailbox& rmb,
                                        model_to_system_map&) {
  memory_access_payload payload = read_memory_access_payload(rmb);
  return new transitions::memory_access(
      p, payload.address, payload.size, payload.site_id,
      transitions::memory_access::kind::read);
}

model::transition* memory_write_callback(runner_id_t p,
                                         const volatile runner_mailbox& rmb,
                                         model_to_system_map&) {
  memory_access_payload payload = read_memory_access_payload(rmb);
  return new transitions::memory_access(
      p, payload.address, payload.size, payload.site_id,
      transitions::memory_access::kind::write);
}
