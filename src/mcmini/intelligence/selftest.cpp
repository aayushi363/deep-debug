// mcmini_intel_selftest — a standalone self-test that links libmcmini_intelligence.so
// and exercises the C ABI on the canonical AB-BA deadlock, so a notebook can
// `podman exec` it to confirm the fuzzer-side DPOR engine builds AND runs
// correctly inside the deployed deep-debug container. Prints one line per
// non-empty backtrack state and a final PASS/FAIL. Exit 0 on PASS.
#include <cstdio>

#include "mcmini/intelligence/mcmini_intelligence.h"

int main(void) {
  // input_deadlock A A B B, minimal model-valid AB-BA interleaving:
  //   main:  init A, init B, create c1, create c2, join c1
  //   c1:    lock A, lock B      c2:    lock B, lock A
  //   schedule: main x5 (thread_start,initA,initB,create1,create2), c2, c1, c2, c1
  const uint64_t A = 0x404040, B = 0x404080;
  mc_op_t r0[] = {{MC_OP_MUTEX_INIT, A},       {MC_OP_MUTEX_INIT, B},
                  {MC_OP_THREAD_CREATE, 1001},  {MC_OP_THREAD_CREATE, 1002},
                  {MC_OP_THREAD_JOIN, 1}};
  mc_op_t r1[] = {{MC_OP_MUTEX_LOCK, A}, {MC_OP_MUTEX_LOCK, B}};
  mc_op_t r2[] = {{MC_OP_MUTEX_LOCK, B}, {MC_OP_MUTEX_LOCK, A}};
  const mc_op_t *ops[3] = {r0, r1, r2};
  uint32_t nops[3] = {5, 2, 2};
  uint32_t sched[] = {0, 0, 0, 0, 0, 2, 1, 2, 1};

  printf("[mcmini_intel_selftest] replaying AB-BA schedule through classic DPOR\n");
  mc_intel_result_t *res = mc_intel_analyze_recorded(3, ops, nops, sched, 9);
  if (!res) {
    printf("[mcmini_intel_selftest] RESULT: FAIL (null result)\n");
    return 1;
  }
  int dl = mc_intel_is_deadlocked(res);
  uint32_t depth = mc_intel_depth(res), ns = mc_intel_num_states(res);
  printf("[mcmini_intel_selftest] deadlocked=%d depth=%u states=%u\n", dl, depth,
         ns);
  size_t total = 0;
  for (uint32_t s = 0; s < ns; s++) {
    uint32_t buf[16];
    size_t n = mc_intel_backtrack_at(res, s, buf, 16);
    total += n;
    if (n) {
      printf("[mcmini_intel_selftest]   state %u ran=%u backtrack={", s,
             mc_intel_ran_at(res, s));
      for (size_t i = 0; i < n; i++) printf("%u ", buf[i]);
      printf("}\n");
    }
  }
  int pass = dl && total > 0;
  printf("[mcmini_intel_selftest] RESULT: %s\n", pass ? "PASS" : "FAIL");
  mc_intel_result_free(res);
  return pass ? 0 : 1;
}
