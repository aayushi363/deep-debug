#pragma once

#include "mcmini/coordinator/model_to_system_map.hpp"
#include "mcmini/model/state.hpp"
#include "mcmini/real_world/mailbox/runner_mailbox.h"
#include "mcmini/model/transitions/barrier/barrier_destroy.hpp"
#include "mcmini/model/transitions/barrier/barrier_init.hpp"
#include "mcmini/model/transitions/barrier/barrier_arrive.hpp"
#include "mcmini/model/transitions/barrier/barrier_wait.hpp"

model::transition* barrier_init_callback(runner_id_t,
                                         const volatile runner_mailbox&,
                                         model_to_system_map&);
model::transition* barrier_arrive_callback(runner_id_t,
                                           const volatile runner_mailbox&,
                                           model_to_system_map&);
model::transition* barrier_pass_callback(runner_id_t,
                                         const volatile runner_mailbox&,
                                         model_to_system_map&);
model::transition* barrier_destroy_callback(runner_id_t,
                                            const volatile runner_mailbox&,
                                            model_to_system_map&);
