#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "mcmini/defines.h"
#include "mcmini/log/severity_level.hpp"

namespace model {

struct config {
  /**
   * The maximum number of transitions that can be run
   * by any _single thread_ while running the model checker
   */
  uint64_t max_thread_execution_depth;

  /**
   * The maximum number of transitions that can be contained in any given trace.
   */
  uint64_t maximum_total_execution_depth = 1500;

  /**
   * The trace id to stop the model checker at
   * to print the contents of the transition stack
   */
  trid_t target_trace_id;

  /**
   * Whether or not exploration occurs using round robin scheduling (default is
   * smallest tid first)
   */
  bool use_round_robin_scheduling = false;

  /**
   * Whether model checking should halt at the first encountered deadlock
   */
  bool stop_at_first_deadlock = false;

  /**
   * Whether to report data races (conflicting, concurrent memory accesses)
   * discovered during model checking. Requires the target to be built with the
   * McMini LLVM memory-instrumentation pass; otherwise no memory-access
   * transitions exist and nothing is reported.
   */
  bool detect_races = false;

  /**
   * Informs McMini that the target executable should be run under DMTCP with
   * `libmcmini.so` configured in record mode.
   */
  bool record_target_executable_only = false;

  /**
   * Informs McMini that model checking with the checkpoint file should occur
   * using `multithreaded_fork()` + a template process instead of
   * `dmtcp_restart` to create new branches
   */
  bool use_multithreaded_fork = false;

  /**
   * The time between consecutive checkpoint images when `libmcmini.so` is
   * running in record mode.
   */
  std::chrono::seconds checkpoint_period;

  /**
   * The path to the checkpoint file that should be used to begin deep debugging
   * from.
   */
  std::string checkpoint_file = "";

  /**
   * The directory where checkpoint files will be stored
   */
  std::string checkpoint_dir = "";

  /*
  * Informs DMTCP to not gzip the checkpoint files
  */
  bool nogzip = false;

  // Name of the target executable that will be model checked
  std::string target_executable = "";

  // A list of arguments to be passed to the executable on launch
  std::vector<std::string> target_executable_args;

  // Default severity level for logging. Overridden if a blacklist/whitelist
  // file path is provided (TODO).
  logging::severity_level global_severity_level = logging::severity_level::info;
};
} // namespace model
