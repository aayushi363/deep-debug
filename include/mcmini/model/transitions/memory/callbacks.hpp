#pragma once

#include "mcmini/coordinator/model_to_system_map.hpp"
#include "mcmini/model/transitions/memory/memory_access.hpp"
#include "mcmini/real_world/mailbox/runner_mailbox.h"

model::transition* memory_read_callback(runner_id_t,
                                        const volatile runner_mailbox&,
                                        model_to_system_map&);
model::transition* memory_write_callback(runner_id_t,
                                         const volatile runner_mailbox&,
                                         model_to_system_map&);
