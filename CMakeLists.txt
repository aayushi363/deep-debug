cmake_minimum_required(VERSION 3.10)
project(McMini-Revamp
    VERSION 1.0.0
    DESCRIPTION "A bite-sized model checker"
    LANGUAGES C CXX)

# Require C11 and C++11
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Project configuration
option(BUILD_TESTS OFF)
option(VERBOSE_TESTING OFF)
option(MCMINI_BUILD_LLVM_PASS "Build the McMini LLVM memory instrumentation pass" ON)
set(MCMINI_DIR "${CMAKE_SOURCE_DIR}")
set(MCMINI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")
set(MCMINI_CMAKE_MODULE_DIR "${CMAKE_SOURCE_DIR}/cmake")
set(MCMINI_WITH_DMTCP OFF)

# Project source files
set(MCMINI_C_SRC
  src/common/exit.c
  src/common/mem.c
  src/common/runner_mailbox.c
  src/common/shm_config.c
)
set(MCMINI_CPP_SRC
  src/mcmini/mcmini.cpp
  src/mcmini/constants.cpp
  src/mcmini/signal.cpp
  src/mcmini/visible_object.cpp
  src/mcmini/coordinator/coordinator.cpp
  src/mcmini/log/filter.cpp
  src/mcmini/log/logger.cpp
  src/mcmini/model/detached_state.cpp
  src/mcmini/model/diff_state.cpp
  src/mcmini/model/program.cpp
  src/mcmini/model/cond_var_arbitrary_policy.cpp
  src/mcmini/model/cond_var_default_policy.cpp
  src/mcmini/model/cond_var_single_grp_policy.cpp
  src/mcmini/model/cond_var_wakegroup.cpp
  src/mcmini/model/state_sequence.cpp
  src/mcmini/model/transition_registry.cpp
  src/mcmini/model/transition_sequence.cpp
  src/mcmini/model/transitions/condition_variables.cpp
  src/mcmini/model/transitions/memory.cpp
  src/mcmini/model/transitions/mutex.cpp
  src/mcmini/model/transitions/process.cpp
  src/mcmini/model/transitions/thread.cpp
  src/mcmini/model/transitions/semaphore.cpp
  src/mcmini/model_checking/algorithms/classic_dpor.cpp
  src/mcmini/model_checking/algorithms/clock_vector.cpp
  src/mcmini/real_world/dmtcp_process_source.cpp
  src/mcmini/real_world/fifo.cpp
  src/mcmini/real_world/fork_process_source.cpp
  src/mcmini/real_world/local_linux_process.cpp
  src/mcmini/real_world/resources.cpp
  src/mcmini/real_world/shm.cpp
  src/mcmini/real_world/target.cpp
)
set(LIBMCMINI_C_SRC
  src/common/exit.c
  src/common/mem.c
  src/common/multithreaded_fork.c
  src/common/runner_mailbox.c
  src/common/shm_config.c
  src/lib/dmtcp-callback.c
  src/lib/entry.c
  src/lib/interception.c
  src/lib/json_events.c
  src/lib/log.c
  src/lib/lockset.c
  src/lib/memory-hooks.c
  src/lib/wrapper_timing.c
  src/lib/main.c
  src/lib/inproc_scheduler.c
  src/lib/deadlock_detector.c
  src/lib/record.c
  src/lib/sem-wrappers.c
  src/lib/template/loop.c
  src/lib/template/sig.c
  src/lib/wrappers.c
  src/mcmini/Thread_queue.c
)

set(MCMINI_EXTRA_COMPILER_FLAGS -Wall)
set(MCMINI_EXTRA_COMPILER_DEFINITIONS "")
set(MCMINI_EXTRA_LINK_FLAGS -lrt -pthread)

set(LIBMCMINI_EXTRA_COMPILER_FLAGS -Wall -fPIC)
set(LIBMCMINI_EXTRA_COMPILER_DEFINITIONS MC_SHARED_LIBRARY=1 DMTCP=1 LOGGING_ROOT="${CMAKE_SOURCE_PATH}")

# -lrt -> shared memory
# -pthread -> libpthread.so
# -lm -> math library
# -ldl -> dlsym etc.
set(LIBMCMINI_EXTRA_LINK_FLAGS -lrt -pthread -lm -ldl)

# libmcmini.so -> the dylib which is loaded
add_library(libmcmini SHARED "${LIBMCMINI_C_SRC}")
set_target_properties(libmcmini PROPERTIES OUTPUT_NAME "mcmini")

target_include_directories(libmcmini PUBLIC "${MCMINI_INCLUDE_DIR}")
target_compile_definitions(libmcmini
  PUBLIC
    "${LIBMCMINI_EXTRA_COMPILER_DEFINITIONS}")
target_compile_options(libmcmini
  PRIVATE
    "${LIBMCMINI_EXTRA_COMPILER_FLAGS}")
target_link_libraries(libmcmini
  PUBLIC
    "${LIBMCMINI_EXTRA_LINK_FLAGS}")

# libmcmini_intelligence.so -> fuzzer-side "split DPOR": mcmini's model + classic
# DPOR compiled WITHOUT the DMTCP/real_world concretes, exposed via a C ABI
# (include/mcmini/intelligence/mcmini_intelligence.h). The Antithesis fuzzer
# links this to reuse mcmini's reduction over schedules the SUT already produced.
# NOTE: this is the model/model_checking/coordinator subset ONLY — no
# real_world/*.cpp (dmtcp/fork process sources) and no mcmini.cpp/signal.cpp.
set(MCMINI_INTEL_SRC
  src/mcmini/constants.cpp
  src/mcmini/visible_object.cpp
  src/mcmini/coordinator/coordinator.cpp
  src/mcmini/log/filter.cpp
  src/mcmini/log/logger.cpp
  src/mcmini/model/detached_state.cpp
  src/mcmini/model/diff_state.cpp
  src/mcmini/model/program.cpp
  src/mcmini/model/cond_var_arbitrary_policy.cpp
  src/mcmini/model/cond_var_default_policy.cpp
  src/mcmini/model/cond_var_single_grp_policy.cpp
  src/mcmini/model/cond_var_wakegroup.cpp
  src/mcmini/model/state_sequence.cpp
  src/mcmini/model/transition_registry.cpp
  src/mcmini/model/transition_sequence.cpp
  src/mcmini/model/transitions/condition_variables.cpp
  src/mcmini/model/transitions/memory.cpp
  src/mcmini/model/transitions/mutex.cpp
  src/mcmini/model/transitions/process.cpp
  src/mcmini/model/transitions/thread.cpp
  src/mcmini/model/transitions/semaphore.cpp
  src/mcmini/model_checking/algorithms/classic_dpor.cpp
  src/mcmini/model_checking/algorithms/clock_vector.cpp
  src/common/mem.c
  src/mcmini/intelligence/mcmini_intelligence.cpp
)
add_library(mcmini_intelligence SHARED "${MCMINI_INTEL_SRC}")
set_target_properties(mcmini_intelligence PROPERTIES OUTPUT_NAME "mcmini_intelligence")
target_include_directories(mcmini_intelligence PUBLIC "${MCMINI_INCLUDE_DIR}")
target_compile_options(mcmini_intelligence PRIVATE -Wall -fPIC)

# mcmini_intel_selftest -> a tiny binary that dynamically links the lib above and
# runs the AB-BA deadlock case through the C ABI. Lets a notebook `podman exec`
# it to confirm the DPOR engine builds AND runs inside the deployed container.
add_executable(mcmini_intel_selftest src/mcmini/intelligence/selftest.cpp)
target_include_directories(mcmini_intel_selftest PUBLIC "${MCMINI_INCLUDE_DIR}")
target_link_libraries(mcmini_intel_selftest mcmini_intelligence)
# Resolve libmcmini_intelligence.so relative to the binary ($ORIGIN) — it is
# installed next to the selftest in /opt/deep-debug/build. Bake this at build
# time so no forbidden /build sandbox path lands in the RPATH (nix fixup fails
# otherwise).
set_target_properties(mcmini_intel_selftest PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "$ORIGIN")

add_executable(mcmini "${MCMINI_CPP_SRC}" "${MCMINI_C_SRC}")
target_include_directories(mcmini PUBLIC "${MCMINI_INCLUDE_DIR}")
target_compile_definitions(mcmini
  PUBLIC
    "${MCMINI_EXTRA_COMPILER_DEFINITIONS}")
target_compile_options(mcmini
  PRIVATE
    "${MCMINI_EXTRA_COMPILER_FLAGS}")
target_link_libraries(mcmini
  PUBLIC
    "${MCMINI_EXTRA_LINK_FLAGS}")

add_subdirectory(src/examples)
add_subdirectory(test)

if(MCMINI_BUILD_LLVM_PASS)
  find_package(LLVM CONFIG QUIET)
  if(LLVM_FOUND)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
    add_library(McMiniMemoryPass MODULE src/llvm/mcmini-memory-pass.cpp)
    target_include_directories(McMiniMemoryPass SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(McMiniMemoryPass PRIVATE ${LLVM_DEFINITIONS})
    target_compile_features(McMiniMemoryPass PRIVATE cxx_std_14)
    set_target_properties(McMiniMemoryPass PROPERTIES
      COMPILE_FLAGS "-fno-rtti"
      PREFIX "")
    find_program(MCMINI_CLANG clang)
    if(MCMINI_CLANG)
      add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/src/examples/data-race-llvm
        COMMAND ${MCMINI_CLANG}
          -g -O0
          -fpass-plugin=$<TARGET_FILE:McMiniMemoryPass>
          ${CMAKE_SOURCE_DIR}/src/examples/data-race-plain.c
          -L$<TARGET_FILE_DIR:libmcmini>
          -Wl,-rpath,$<TARGET_FILE_DIR:libmcmini>
          -lmcmini
          -pthread
          -o ${CMAKE_BINARY_DIR}/src/examples/data-race-llvm
        DEPENDS
          McMiniMemoryPass
          libmcmini
          ${CMAKE_SOURCE_DIR}/src/examples/data-race-plain.c)
      add_custom_target(data-race-llvm ALL
        DEPENDS ${CMAKE_BINARY_DIR}/src/examples/data-race-llvm)
    else()
      message(WARNING "clang was not found; data-race-llvm example will not be built")
    endif()
  else()
    message(WARNING "LLVM was not found; McMiniMemoryPass will not be built")
  endif()
endif()
