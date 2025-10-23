add_executable(hello-world hello-world.c)
add_executable(cv-hello-world cv-hello-world.c)
add_executable(cv-test cv-test.c)
add_executable(mutex-uninitialized mutex-uninitialized.c)
add_executable(cv-uninitialized cv-uninitialized.c)
add_executable(deadly-embrace deadly-embrace.c)
add_executable(fifo-example fifo.cpp)
add_executable(producer-consumer producer-consumer.c)
add_executable(producer-consumer-safe producer-consumer-safe.c)
add_executable(producer-consumer-exit producer-consumer-exit.c)
add_executable(producer-consumer-park producer-consumer-park.c)
add_executable(exit-stress exit-stress.c)
add_executable(cv-producer-consumer cv-producer-consumer.c)
add_executable(cv-producer-consumer-safe cv-producer-consumer-safe.c)
add_executable(dining_phil dining_phil.c)
add_executable(subtle subtle.c)
add_executable(ABA_example ABA_example.c)
target_link_libraries(hello-world PUBLIC -pthread)
target_link_libraries(cv-hello-world PUBLIC -pthread)
target_link_libraries(cv-test PUBLIC -pthread)
target_link_libraries(mutex-uninitialized PUBLIC -pthread)
target_link_libraries(cv-uninitialized PUBLIC -pthread)
target_link_libraries(deadly-embrace PUBLIC -pthread)
target_link_libraries(producer-consumer PUBLIC -pthread)
target_link_libraries(producer-consumer-safe PUBLIC -pthread)
target_link_libraries(producer-consumer-exit PUBLIC -pthread)
target_link_libraries(producer-consumer-park PUBLIC -pthread)
target_link_libraries(exit-stress PUBLIC -pthread)
target_link_libraries(cv-producer-consumer PUBLIC -pthread)
target_link_libraries(cv-producer-consumer-safe PUBLIC -pthread)

# TSan-instrumented variant of producer-consumer, for testing McMini's
# DMTCP-restart + TSan integration (see CLAUDE.md, "DMTCP-restart under
# ThreadSanitizer"). Needs -Wl,--wrap=pthread_join plus
# src/lib/pthread_join_wrap.c compiled directly into this binary -- see
# that file's header comment for why it can't live in libmcmini.so itself.
add_executable(producer-consumer-tsan producer-consumer.c
                                      ../lib/pthread_join_wrap.c)
target_compile_options(producer-consumer-tsan PUBLIC -fsanitize=thread)
target_link_options(producer-consumer-tsan PUBLIC -fsanitize=thread
                                                   -Wl,--wrap=pthread_join)
target_link_libraries(producer-consumer-tsan PUBLIC -pthread libmcmini)

# do_recording() (src/mcmini/mcmini.cpp) resolves the plugin path as
# getcwd()/libmcmini.so, so mcmini must be invoked from the directory holding
# libmcmini.so -- i.e. CMAKE_BINARY_DIR, not this target's own output
# directory. Keep a copy there in sync on every build, so it can't silently
# go stale relative to libmcmini.so.
add_custom_command(TARGET producer-consumer-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:producer-consumer-tsan>
          ${CMAKE_BINARY_DIR}/producer-consumer-tsan)

# Same as producer-consumer-tsan, but built from producer-consumer-safe.c --
# identical except for a fixed pre-existing data race in main() (each thread
# gets its own argument slot instead of sharing one reused array). Use this
# target when a test needs to rule out that race, so it can focus on other
# bugs (see doc/cond-wait-tsan-interceptor-bypass.txt's "Follow-on issue").
add_executable(producer-consumer-safe-tsan producer-consumer-safe.c
                                           ../lib/pthread_join_wrap.c)
target_compile_options(producer-consumer-safe-tsan PUBLIC -fsanitize=thread)
target_link_options(producer-consumer-safe-tsan PUBLIC -fsanitize=thread
                                                        -Wl,--wrap=pthread_join)
target_link_libraries(producer-consumer-safe-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET producer-consumer-safe-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:producer-consumer-safe-tsan>
          ${CMAKE_BINARY_DIR}/producer-consumer-safe-tsan)

# Same as producer-consumer-safe-tsan, but worker threads terminate via an
# explicit pthread_exit() call instead of returning -- deliberately WITHOUT
# a pthread_exit wrap yet, to test whether that path has the same
# libtsan-loads-ahead-of-libmcmini load-order bypass already fixed for
# pthread_join/pthread_cond_wait (see those files' header comments). Our own
# pthread_exit is currently defined inside libmcmini.so itself
# (src/lib/interception.c), not compiled into the target the way join/
# cond_wait are, which is exactly the pattern that bypass needs.
add_executable(producer-consumer-exit-tsan producer-consumer-exit.c
                                           ../lib/pthread_join_wrap.c)
target_compile_options(producer-consumer-exit-tsan PUBLIC -fsanitize=thread)
target_link_options(producer-consumer-exit-tsan PUBLIC -fsanitize=thread
                                                        -Wl,--wrap=pthread_join)
target_link_libraries(producer-consumer-exit-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET producer-consumer-exit-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:producer-consumer-exit-tsan>
          ${CMAKE_BINARY_DIR}/producer-consumer-exit-tsan)

# producer-consumer-park.c never calls pthread_join() or pthread_exit() --
# main returns while both workers are still alive, parked forever on an
# unposted semaphore (see that file's own comment, modeled on
# multithreaded-fork-tsan-2.0's test_park.c). No --wrap needed: neither
# pthread_join nor pthread_cond_wait is ever called here.
add_executable(producer-consumer-park-tsan producer-consumer-park.c)
target_compile_options(producer-consumer-park-tsan PUBLIC -fsanitize=thread)
target_link_options(producer-consumer-park-tsan PUBLIC -fsanitize=thread)
target_link_libraries(producer-consumer-park-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET producer-consumer-park-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:producer-consumer-park-tsan>
          ${CMAKE_BINARY_DIR}/producer-consumer-park-tsan)

# Stress test for pthread_exit() robustness at higher thread counts (see
# exit-stress.c) -- same build recipe as producer-consumer-exit-tsan above.
add_executable(exit-stress-tsan exit-stress.c
                                ../lib/pthread_join_wrap.c)
target_compile_options(exit-stress-tsan PUBLIC -fsanitize=thread)
target_link_options(exit-stress-tsan PUBLIC -fsanitize=thread
                                            -Wl,--wrap=pthread_join)
target_link_libraries(exit-stress-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET exit-stress-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:exit-stress-tsan>
          ${CMAKE_BINARY_DIR}/exit-stress-tsan)

# Same build recipe as exit-stress-tsan above. Reliably (4/4 attempts)
# reproduces a genuine checkpoint-thread/TSAN-background-thread startup
# race, distinct from the restoreBuf={0,0} bug this was originally meant to
# help isolate -- see exit-stress-noop.c's own header comment.
add_executable(exit-stress-noop-tsan exit-stress-noop.c
                                     ../lib/pthread_join_wrap.c)
target_compile_options(exit-stress-noop-tsan PUBLIC -fsanitize=thread)
target_link_options(exit-stress-noop-tsan PUBLIC -fsanitize=thread
                                                 -Wl,--wrap=pthread_join)
target_link_libraries(exit-stress-noop-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET exit-stress-noop-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:exit-stress-noop-tsan>
          ${CMAKE_BINARY_DIR}/exit-stress-noop-tsan)

# TSan-instrumented variant of cv-producer-consumer, for testing McMini's
# DMTCP-restart + TSan integration against condition variables (see
# doc/glibc-cond-var-desync.txt). Same recipe as producer-consumer-tsan
# above, plus -Wl,--wrap=pthread_cond_wait + pthread_cond_wait_wrap.c and
# -Wl,--wrap=pthread_cond_signal + pthread_cond_signal_wrap.c -- see those
# files' header comments for why each is needed.
add_executable(cv-producer-consumer-tsan cv-producer-consumer.c
                                         ../lib/pthread_join_wrap.c
                                         ../lib/pthread_cond_wait_wrap.c
                                         ../lib/pthread_cond_signal_wrap.c)
target_compile_options(cv-producer-consumer-tsan PUBLIC -fsanitize=thread)
target_link_options(cv-producer-consumer-tsan PUBLIC
  -fsanitize=thread
  -Wl,--wrap=pthread_join
  -Wl,--wrap=pthread_cond_wait
  -Wl,--wrap=pthread_cond_signal)
target_link_libraries(cv-producer-consumer-tsan PUBLIC -pthread libmcmini)

# See the producer-consumer-tsan POST_BUILD step above for why this is needed.
add_custom_command(TARGET cv-producer-consumer-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:cv-producer-consumer-tsan>
          ${CMAKE_BINARY_DIR}/cv-producer-consumer-tsan)

# Same as cv-producer-consumer-tsan, but built from cv-producer-consumer-safe.c
# -- see producer-consumer-safe-tsan above for why.
add_executable(cv-producer-consumer-safe-tsan cv-producer-consumer-safe.c
                                              ../lib/pthread_join_wrap.c
                                              ../lib/pthread_cond_wait_wrap.c
                                              ../lib/pthread_cond_signal_wrap.c)
target_compile_options(cv-producer-consumer-safe-tsan PUBLIC -fsanitize=thread)
target_link_options(cv-producer-consumer-safe-tsan PUBLIC
  -fsanitize=thread
  -Wl,--wrap=pthread_join
  -Wl,--wrap=pthread_cond_wait
  -Wl,--wrap=pthread_cond_signal)
target_link_libraries(cv-producer-consumer-safe-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET cv-producer-consumer-safe-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:cv-producer-consumer-safe-tsan>
          ${CMAKE_BINARY_DIR}/cv-producer-consumer-safe-tsan)

# Same as cv-producer-consumer-safe-tsan, but with multiple producers and
# multiple consumers (see cv-producer-consumer-multi.c for why).
add_executable(cv-producer-consumer-multi-tsan
  cv-producer-consumer-multi.c
  ../lib/pthread_join_wrap.c
  ../lib/pthread_cond_wait_wrap.c
  ../lib/pthread_cond_signal_wrap.c)
target_compile_options(cv-producer-consumer-multi-tsan PUBLIC -fsanitize=thread)
target_link_options(cv-producer-consumer-multi-tsan PUBLIC
  -fsanitize=thread
  -Wl,--wrap=pthread_join
  -Wl,--wrap=pthread_cond_wait
  -Wl,--wrap=pthread_cond_signal)
target_link_libraries(cv-producer-consumer-multi-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET cv-producer-consumer-multi-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:cv-producer-consumer-multi-tsan>
          ${CMAKE_BINARY_DIR}/cv-producer-consumer-multi-tsan)

target_link_libraries(dining_phil PUBLIC -pthread)
target_link_libraries(subtle PUBLIC -pthread)
target_link_libraries(ABA_example PUBLIC -pthread)
