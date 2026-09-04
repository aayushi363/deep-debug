// inproc_scheduler.c
//
// The in-SUT runner_mailbox scheduler for clean-split FUZZER_STANDALONE mode
// (M2.1). A dedicated scheduler thread runs the classic single-step loop
// entirely inside the SUT process: every runner (main + pthread_create'd
// threads) parks in its mailbox at each mutex/thread op; the scheduler computes
// the *enabled* set (operational safety rail — which ops won't block), asks the
// fuzzer WHICH enabled runner to release via fuzz_getchar() (idx = byte %
// enabled), releases exactly that one, and advances. When the enabled set is
// empty but runners remain parked -> real deadlock (emit MCMINI_SCHEDULE_JSON,
// outcome=deadlock). NO DPOR here; the decision byte comes from the fuzzer.
//
// Scope (M2.1): mutex_init/lock/unlock + pthread_create/join/exit. cond/sem
// still use the old proceed/abort path in their wrappers.

#define _GNU_SOURCE
#include "mcmini/lib/inproc_scheduler.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mcmini/mcmini.h"

// --- externs from libmcmini we reuse -------------------------------------
extern volatile void *global_shm_start;                  // entry.h
extern runner_id_t mc_register_this_thread(void);        // wrappers.c
extern int libpthread_pthread_create(pthread_t *, const pthread_attr_t *,
                                     void *(*)(void *), void *);  // interception.c
// The wrappers' "already inside a wrapper — pass through to libpthread" flag
// (defined in json_events.c). We set it on the scheduler thread so its own
// pthread ops (e.g. a mutex taken internally by libvoidstar's fuzz_getchar
// under the campaign) bypass the mailbox path instead of asserting on the
// scheduler's RID_INVALID tid.
extern __thread int mc_in_wrapper;
// The decision channel. Weak: resolves to libvoidstar's VMCALL under the
// campaign, or getchar_stub/stdin standalone. Guarded below in case unresolved.
extern int fuzz_getchar(void) __attribute__((weak));

// Antithesis assertion emit channel — the same libvoidstar the C++ SDK uses
// (antithesis_sdk.h: fuzz_json_data(json, strlen(json)); fuzz_flush()). Weak,
// like fuzz_getchar: a standalone run without libvoidstar simply skips it.
extern void fuzz_json_data(const char *data, size_t size) __attribute__((weak));
extern void fuzz_flush(void) __attribute__((weak));

// glibc: basename of argv[0] (e.g. "sct_sync02"). Used to make the deadlock
// triage property PER-BENCHMARK — each SUT binary gets its own named property,
// so a sweep over many benchmarks shows one FAILED property per benchmark
// instead of all collapsing into one shared "deadlock" property.
extern char *program_invocation_short_name;

// emit_deadlock_property() is defined below, after the schedule serializers
// (g_events / g_pending / g_recv) whose buffers it reads to attach the deadlock
// trace to the assertion details. See just after sched_emit().

// --- scheduler-local state (touched only by the scheduler thread) --------
typedef enum {
  RS_NONE = 0,     // slot unused
  RS_READY_START,  // freshly created; not yet run its first op (always enabled)
  RS_PARKED,       // announced a pending op, waiting for the scheduler
  RS_RUNNING,      // released; executing toward its next op
  RS_EXITED,       // done
} rstatus_t;

typedef struct {
  rstatus_t status;
  uint32_t op;         // pending op type (transitions.h) when RS_PARKED
  void *obj;           // mutex ptr for MUTEX_*; sem ptr for SEM_*; cond ptr for COND_*
  runner_id_t target;  // target runner for THREAD_JOIN; init count for SEM_INIT
  void *cond_mutex;    // associated mutex for COND_ENQUEUE / COND_WAIT
  int cond_woken;      // 1 once a signal/broadcast wakes this enqueued cv waiter
} runner_state_t;

static runner_state_t g_rs[MAX_TOTAL_THREADS_IN_PROGRAM];
static runner_id_t g_max_rid = 0;

// Mutex ownership table (linear; object identity = pointer value).
typedef struct { void *m; int locked; runner_id_t owner; } mutex_state_t;
static mutex_state_t g_mtab[MAX_TOTAL_THREADS_IN_PROGRAM * 4];
static int g_mtab_n = 0;

// Running fingerprint of the schedule (chosen runner + op per step), so that
// different interleavings yield different trace_ids.
static uint64_t g_hash = 14695981039346656037ULL;  // FNV-1a basis
static unsigned long g_steps = 0;
// Set once a summary is emitted (deadlock in the loop, or clean at exit) so the
// two paths never double-emit for one run.
static volatile int g_outcome_emitted = 0;

#define OP_THREAD_START_SENTINEL 0xFFFFFFFFu

static int sched_debug(void) {
  static int v = -1;
  if (v < 0) v = getenv("MCMINI_SCHED_DEBUG") ? 1 : 0;
  return v;
}
#define SDBG(...) do { if (sched_debug()) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while (0)

static int sched_getchar(void) {
  if (fuzz_getchar) return fuzz_getchar();
  return 0;  // no provider -> deterministic idx 0
}

static volatile runner_mailbox *mb_of(runner_id_t r) {
  return &((volatile struct mcmini_shm_file *)global_shm_start)->mailboxes[r];
}

static int mtab_find(void *m) {
  for (int i = 0; i < g_mtab_n; i++)
    if (g_mtab[i].m == m) return i;
  return -1;
}
static void mtab_set(void *m, int locked, runner_id_t owner) {
  int i = mtab_find(m);
  if (i < 0) {
    if (g_mtab_n >= (int)(sizeof g_mtab / sizeof g_mtab[0])) return;
    i = g_mtab_n++;
    g_mtab[i].m = m;
  }
  g_mtab[i].locked = locked;
  g_mtab[i].owner = owner;
}

// Semaphore count table (linear; object identity = pointer value). Mirrors each
// semaphore's count so the scheduler can decide sem_wait enabledness (enabled
// iff count > 0) and detect sem deadlocks (all runnable threads blocked on a
// zero-count wait). Populated from the observed SEM_INIT and each POST/WAIT.
typedef struct { void *s; int count; } sem_state_t;
static sem_state_t g_stab[MAX_TOTAL_THREADS_IN_PROGRAM * 4];
static int g_stab_n = 0;
static int stab_find(void *s) {
  for (int i = 0; i < g_stab_n; i++)
    if (g_stab[i].s == s) return i;
  return -1;
}
static void stab_set(void *s, int count) {
  int i = stab_find(s);
  if (i < 0) {
    if (g_stab_n >= (int)(sizeof g_stab / sizeof g_stab[0])) return;
    i = g_stab_n++;
    g_stab[i].s = s;
  }
  g_stab[i].count = count;
}
static int stab_count(void *s) {
  int i = stab_find(s);
  return (i < 0) ? 0 : g_stab[i].count;
}

static void hash_step(runner_id_t r, uint32_t op) {
  g_hash = (g_hash ^ (uint64_t)r) * 1099511628211ULL;
  g_hash = (g_hash ^ (uint64_t)op) * 1099511628211ULL;
  g_steps++;
}

// --- schedule recording: capture each step's FULL enabled set (every runnable
//     runner + its pending op + object), in canonical (ascending runner-id)
//     order, alongside which runner was chosen. This is exactly the trace the
//     fuzzer-side DPOR (mcmini_intelligence) ingests: classic DPOR needs each
//     parked thread's *pending* transition to compute the dependency/coenabled
//     relations and backtrack sets — not just the transition that ran. Emitting
//     only the chosen op (the old behavior) would blind DPOR to the alternatives
//     it must explore. ---
static char g_events[1 << 16];
static int g_events_len = 0;

// Human-readable execution trace (one "thread R: pthread_...(obj)" line per
// scheduled step, in order) — the readable companion to g_events, attached to
// the deadlock assertion's details so the failing example reads like a schedule.
static char g_trace[1 << 15];
static int g_trace_len = 0;

static const char *op_name(uint32_t op) {
  switch (op) {
    case MUTEX_INIT_TYPE:          return "mutex_init";
    case MUTEX_LOCK_TYPE:          return "mutex_lock";
    case MUTEX_UNLOCK_TYPE:        return "mutex_unlock";
    case THREAD_CREATE_TYPE:       return "thread_create";
    case THREAD_JOIN_TYPE:         return "thread_join";
    case THREAD_EXIT_TYPE:         return "thread_exit";
    case SEM_INIT_TYPE:            return "sem_init";
    case SEM_WAIT_TYPE:            return "sem_wait";
    case SEM_POST_TYPE:            return "sem_post";
    case SEM_DESTROY_TYPE:         return "sem_destroy";
    case COND_ENQUEUE_TYPE:        return "cond_enqueue";
    case COND_WAIT_TYPE:           return "cond_wait";
    case COND_SIGNAL_TYPE:         return "cond_signal";
    case COND_BROADCAST_TYPE:      return "cond_broadcast";
    case COND_INIT_TYPE:           return "cond_init";
    case COND_DESTROY_TYPE:        return "cond_destroy";
    case OP_THREAD_START_SENTINEL: return "thread_start";
    default:                       return "other";
  }
}

// Pending transition kind of a runner: a freshly-created-but-not-yet-run thread
// is modeled as a THREAD_START (its first real op is not known until it runs);
// otherwise it is the op the runner parked on.
static uint32_t pending_op_of(runner_id_t r) {
  return (g_rs[r].status == RS_READY_START) ? OP_THREAD_START_SENTINEL : g_rs[r].op;
}

// Render one transition as a human-readable pthread call, e.g.
// "pthread_mutex_lock(mutex 0x404040)" — used for the readable trace/blocked
// lists in the deadlock assertion details.
// pthread_cond_wait is modeled as two transitions (COND_ENQUEUE = sleep+release
// mutex, COND_WAIT = wake+reacquire mutex). `in_blocked` renders a thread that
// is PARKED on the wait (i.e. asleep waiting for a signal) as "asleep in ..."
// rather than "wake ...", which would misdescribe a stuck waiter.
static void readable_op(char *out, size_t cap, uint32_t op, void *obj,
                        runner_id_t target, int in_blocked) {
  switch (op) {
    case MUTEX_INIT_TYPE:    snprintf(out, cap, "pthread_mutex_init(mutex %p)", obj); break;
    case MUTEX_LOCK_TYPE:    snprintf(out, cap, "pthread_mutex_lock(mutex %p)", obj); break;
    case MUTEX_UNLOCK_TYPE:  snprintf(out, cap, "pthread_mutex_unlock(mutex %p)", obj); break;
    case THREAD_CREATE_TYPE: snprintf(out, cap, "pthread_create()"); break;
    case THREAD_JOIN_TYPE:   snprintf(out, cap, "pthread_join(thread %u)", (unsigned)target); break;
    case THREAD_EXIT_TYPE:   snprintf(out, cap, "pthread_exit()"); break;
    case OP_THREAD_START_SENTINEL: snprintf(out, cap, "thread start"); break;
    case SEM_INIT_TYPE:      snprintf(out, cap, "sem_init(sem %p)", obj); break;
    case SEM_WAIT_TYPE:      snprintf(out, cap, "sem_wait(sem %p)", obj); break;
    case SEM_POST_TYPE:      snprintf(out, cap, "sem_post(sem %p)", obj); break;
    case SEM_DESTROY_TYPE:   snprintf(out, cap, "sem_destroy(sem %p)", obj); break;
    case COND_INIT_TYPE:     snprintf(out, cap, "pthread_cond_init(cond %p)", obj); break;
    case COND_ENQUEUE_TYPE:  snprintf(out, cap, "pthread_cond_wait(cond %p) - sleep (release mutex)", obj); break;
    case COND_WAIT_TYPE:
      if (in_blocked) snprintf(out, cap, "asleep in pthread_cond_wait(cond %p)", obj);
      else            snprintf(out, cap, "pthread_cond_wait(cond %p) - wake (reacquire mutex)", obj);
      break;
    case COND_SIGNAL_TYPE:   snprintf(out, cap, "pthread_cond_signal(cond %p)", obj); break;
    case COND_BROADCAST_TYPE:snprintf(out, cap, "pthread_cond_broadcast(cond %p)", obj); break;
    case COND_DESTROY_TYPE:  snprintf(out, cap, "pthread_cond_destroy(cond %p)", obj); break;
    default:                 snprintf(out, cap, "%s(%p)", op_name(op), obj); break;
  }
}

// Append one "thread R: <call>" entry as a JSON string element to (buf,*len).
// in_blocked=1 for the blocked/parked list (renders a stuck waiter as asleep).
static void append_readable(char *buf, int *len, int cap, runner_id_t r,
                            uint32_t op, void *obj, runner_id_t target,
                            int in_blocked) {
  int rem = cap - *len;
  if (rem < 160) return;
  char call[112];
  readable_op(call, sizeof call, op, obj, target, in_blocked);
  int n = snprintf(buf + *len, (size_t)rem, "%s\"thread %u: %s\"",
                   *len ? "," : "", (unsigned)r, call);
  if (n > 0 && n < rem) *len += n;
}

// Append one enabled runner's pending transition {runner,op,obj[,target]}.
// `idx` is its position in the enabled array (drives the JSON comma).
static void record_enabled_entry(int idx, runner_id_t r) {
  int rem = (int)sizeof(g_events) - g_events_len;
  if (rem < 256) return;
  uint32_t op = pending_op_of(r);
  int n;
  if (op == THREAD_JOIN_TYPE)
    n = snprintf(g_events + g_events_len, (size_t)rem,
                 "%s{\"runner\":%u,\"op\":\"%s\",\"obj\":\"%p\",\"target\":%u}",
                 idx ? "," : "", (unsigned)r, op_name(op), g_rs[r].obj,
                 (unsigned)g_rs[r].target);
  else
    n = snprintf(g_events + g_events_len, (size_t)rem,
                 "%s{\"runner\":%u,\"op\":\"%s\",\"obj\":\"%p\"}",
                 idx ? "," : "", (unsigned)r, op_name(op), g_rs[r].obj);
  if (n > 0 && n < rem) g_events_len += n;
}

// Append one scheduling STEP: the chosen transition plus the full enabled set.
//   {"step":N,"runner":C,"op":..,"obj":..,"enabled":[{runner,op,obj},...]}
// `enabled`/`ne` are the scheduler's canonical-order candidate set at this state.
static void record_step(const runner_id_t *enabled, int ne, runner_id_t chosen,
                        uint32_t chosen_op, void *chosen_obj) {
  int rem = (int)sizeof(g_events) - g_events_len;
  if (rem < 2048) return;  // near full: stop recording (the schedule still runs)
  int n;
  if (chosen_op == SEM_INIT_TYPE)
    // Emit the init count so the fuzzer-side model can seed the sem's count
    // (needed for sem_wait enabledness). g_rs[chosen].target holds it (read_op).
    n = snprintf(g_events + g_events_len, (size_t)rem,
                 "%s{\"step\":%lu,\"runner\":%u,\"op\":\"%s\",\"obj\":\"%p\",\"count\":%u,\"enabled\":[",
                 g_events_len ? "," : "", g_steps, (unsigned)chosen,
                 op_name(chosen_op), chosen_obj, (unsigned)g_rs[chosen].target);
  else
    n = snprintf(g_events + g_events_len, (size_t)rem,
                 "%s{\"step\":%lu,\"runner\":%u,\"op\":\"%s\",\"obj\":\"%p\",\"enabled\":[",
                 g_events_len ? "," : "", g_steps, (unsigned)chosen,
                 op_name(chosen_op), chosen_obj);
  if (n < 0 || n >= rem) return;
  g_events_len += n;
  for (int i = 0; i < ne; i++) record_enabled_entry(i, enabled[i]);
  rem = (int)sizeof(g_events) - g_events_len;
  if (rem > 4) {
    n = snprintf(g_events + g_events_len, (size_t)rem, "]}");
    if (n > 0 && n < rem) g_events_len += n;
  }
  // Readable companion: "thread C: <call>" for the chosen transition.
  append_readable(g_trace, &g_trace_len, (int)sizeof(g_trace), chosen, chosen_op,
                  chosen_obj, g_rs[chosen].target, /*in_blocked=*/0);
}

// The "pending" set at a deadlock: every still-parked runner's blocked (and
// therefore disabled) op. b1's per-step enabled set omits disabled runners, so
// this is the ONLY record of the ops the stuck threads are waiting on — the
// fuzzer-side DPOR needs it to reproduce the deadlock (distinguish it from a
// clean end). Empty for clean runs.
static char g_pending[4096];
// Readable companion to g_pending: "thread R: <call>" for each stuck thread.
static char g_blocked[4096];

static void build_pending(void) {
  g_pending[0] = '\0';
  g_blocked[0] = '\0';
  int len = 0, blen = 0;
  for (runner_id_t r = 0; r <= g_max_rid; r++) {
    if (g_rs[r].status != RS_PARKED) continue;  // exited/ready aren't blocked
    int rem = (int)sizeof(g_pending) - len;
    if (rem < 128) break;
    int n;
    if (g_rs[r].op == THREAD_JOIN_TYPE)
      n = snprintf(g_pending + len, (size_t)rem,
                   "%s{\"runner\":%u,\"op\":\"%s\",\"obj\":\"%p\",\"target\":%u}",
                   len ? "," : "", (unsigned)r, op_name(g_rs[r].op), g_rs[r].obj,
                   (unsigned)g_rs[r].target);
    else
      n = snprintf(g_pending + len, (size_t)rem,
                   "%s{\"runner\":%u,\"op\":\"%s\",\"obj\":\"%p\"}",
                   len ? "," : "", (unsigned)r, op_name(g_rs[r].op), g_rs[r].obj);
    if (n > 0 && n < rem) len += n;
    append_readable(g_blocked, &blen, (int)sizeof(g_blocked), r, g_rs[r].op,
                    g_rs[r].obj, g_rs[r].target, /*in_blocked=*/1);
  }
}

// Raw decision bytes received from sched_getchar (one per step), recorded so we
// can see PER SCHEDULE exactly what the SUT received. Diagnostic for "why did a
// rollout run a different schedule than the all-0 we sent?": non-0 recv => the
// byte channel delivered non-0 (fuzzer changed the input); all-0 recv but a
// different schedule => SUT-side nondeterminism (enabled order / rid assignment).
static char g_recv[8192];
static int g_recv_len = 0;
static void record_recv(int byte) {
  int rem = (int)sizeof(g_recv) - g_recv_len;
  if (rem < 16) return;
  int n = snprintf(g_recv + g_recv_len, (size_t)rem, "%s%d",
                   g_recv_len ? "," : "", byte);
  if (n > 0 && n < rem) g_recv_len += n;
}

// Emit the schedule summary DeepDebugStrategy parses (deep_debug_strategy.rs:44).
// trace_id derives from the fingerprint so distinct interleavings differ.
static void sched_emit(const char *outcome) {
  g_outcome_emitted = 1;
  static char buf[(1 << 16) + 4096];  // static: must exceed g_events + JSON frame
  long long tid = (long long)(g_hash & 0x7fffffffffffffffULL);
  int n = snprintf(buf, sizeof buf,
                   "MCMINI_SCHEDULE_JSON: {\"mcmini_schedule\":{"
                   "\"events\":[%s],\"outcome\":\"%s\",\"pending\":[%s],"
                   "\"recv\":[%s],"
                   "\"stats\":{\"total_transitions\":%lu,\"object_count\":%d},"
                   "\"trace_id\":%lld}}\n",
                   g_events, outcome, g_pending, g_recv, g_steps, g_mtab_n, tid);
  if (n > 0) (void)write(2, buf, (size_t)n);
}

// Emit the "deadlock is unreachable" Antithesis property over the libvoidstar
// VMCALL so it surfaces in triage. A reachability/Unreachable assertion FAILS
// when hit, so a hit == "the SUT reached a deadlock (a bug)". Called two ways:
//   hit=0 — CATALOG registration: lists the property in triage (details {})
//           without failing it. Done ONCE at a safe point (after the first
//           fuzz_getchar; see the call site), never from a constructor —
//           fuzz_json_data is not ready before the guest's first syncio and
//           registering that early CRASHES the SUT.
//   hit=1 — at the deadlock site: flips the property to FAILED and attaches the
//           deadlock TRACE in details — a readable schedule (trace: "thread R:
//           pthread_...()" per step), the blocked/deadlocked threads (blocked),
//           and trace_id — so the failing example in triage reads as the exact
//           schedule that deadlocked. Reads g_trace / g_blocked; call it AFTER
//           build_pending().
// id == message == "deep-debug detected a deadlock: <binary>" (per-benchmark, so
// a sweep shows one property per binary); triage keys one property per id.
static void emit_deadlock_property(int hit) {
  if (!fuzz_json_data) return;  // no libvoidstar (standalone) — skip
  static char det[(1 << 16) + (1 << 14)];  // static: details body (may hold full events)
  static char buf[(1 << 16) + (1 << 15)];  // static: the full assertion JSON
  det[0] = '\0';
  if (hit) {
    long long tid = (long long)(g_hash & 0x7fffffffffffffffULL);
    // Human-readable trace: the schedule as "thread R: pthread_...()" lines plus
    // the blocked (deadlocked) threads. (The raw decision bytes stay in the
    // MCMINI_SCHEDULE_JSON log for replay; they're just noise in the report.)
    int dn = snprintf(det, sizeof det,
        "\"outcome\":\"deadlock\",\"trace_id\":%lld,\"total_transitions\":%lu,"
        "\"trace\":[%s],\"blocked\":[%s]",
        tid, g_steps, g_trace, g_blocked);
    if (dn <= 0 || (size_t)dn >= sizeof det)
      // trace too large to embed: keep the blocked threads + ids, drop trace.
      snprintf(det, sizeof det,
          "\"outcome\":\"deadlock\",\"trace_id\":%lld,\"total_transitions\":%lu,"
          "\"blocked\":[%s]",
          tid, g_steps, g_blocked);
  }
  // Per-benchmark property: name it after the SUT binary so a sweep shows one
  // FAILED property per benchmark, not one shared "deadlock" property.
  const char *bench = program_invocation_short_name ? program_invocation_short_name
                                                    : "unknown";
  char prop[160];
  snprintf(prop, sizeof prop, "deep-debug detected a deadlock: %s", bench);
  int n = snprintf(buf, sizeof buf,
      "{\"antithesis_assert\":{\"hit\":%s,\"must_hit\":false,"
      "\"assert_type\":\"reachability\",\"display_type\":\"Unreachable\","
      "\"message\":\"%s\",\"condition\":false,"
      "\"id\":\"%s\","
      "\"location\":{\"class\":\"\",\"function\":\"scheduler_main\","
      "\"file\":\"inproc_scheduler.c\",\"begin_line\":0,\"begin_column\":0},"
      "\"details\":{%s}}}",
      hit ? "true" : "false", prop, prop, det);
  if (n > 0 && (size_t)n < sizeof buf) {
    fuzz_json_data(buf, (size_t)n);
    if (fuzz_flush) fuzz_flush();
  }
}

// atexit hook: fires on NORMAL process exit — i.e. main() returned after the
// scheduler drove a full schedule with no deadlock. The deadlock path uses
// _exit(), which skips atexit, so reaching here un-emitted means the run was
// clean. Emit it so DeepDebugStrategy records the clean equivalence class too
// (the schedule-derived trace_id distinguishes it from other interleavings) —
// otherwise clean rollouts are silent and only deadlocks get recorded.
static void sched_emit_clean_atexit(void) {
  if (!g_outcome_emitted) sched_emit("clean");
}

// Read the op a runner just announced (its mailbox holds type + payload).
static void read_op(runner_id_t r) {
  volatile runner_mailbox *mb = mb_of(r);
  uint32_t t = mb->type;
  g_rs[r].op = t;
  g_rs[r].status = RS_PARKED;
  g_rs[r].obj = NULL;
  g_rs[r].target = RID_INVALID;
  if (t == MUTEX_LOCK_TYPE || t == MUTEX_UNLOCK_TYPE || t == MUTEX_INIT_TYPE) {
    void *obj = NULL;
    memcpy(&obj, (const void *)mb->cnts, sizeof obj);
    g_rs[r].obj = obj;
  } else if (t == THREAD_JOIN_TYPE) {
    runner_id_t tgt = RID_INVALID;
    memcpy(&tgt, (const void *)mb->cnts, sizeof tgt);
    g_rs[r].target = tgt;
  } else if (t == THREAD_CREATE_TYPE) {
    // Record the child rid but DON'T make it runnable yet. A child can only
    // start once main's thread_create transition actually EXECUTES — matching
    // mcmini's model and real pthread semantics (a child can't run before
    // pthread_create returns). step_runner activates it on the create step.
    // (The child thread has already registered + parked in the create
    // handshake; this only controls when the scheduler may wake it.)
    runner_id_t child = RID_INVALID;
    memcpy(&child, (const void *)mb->cnts, sizeof child);
    g_rs[r].target = child;
  } else if (t == SEM_INIT_TYPE) {
    // payload: sem ptr, then the unsigned init count. Stash the count in
    // `target` (unused for sem ops) so step_runner can seed the count table.
    void *obj = NULL;
    unsigned cnt = 0;
    memcpy(&obj, (const void *)mb->cnts, sizeof obj);
    memcpy(&cnt, (const void *)(mb->cnts + sizeof obj), sizeof cnt);
    g_rs[r].obj = obj;
    g_rs[r].target = (runner_id_t)cnt;
  } else if (t == SEM_WAIT_TYPE || t == SEM_POST_TYPE || t == SEM_DESTROY_TYPE) {
    void *obj = NULL;
    memcpy(&obj, (const void *)mb->cnts, sizeof obj);
    g_rs[r].obj = obj;
  } else if (t == COND_ENQUEUE_TYPE || t == COND_WAIT_TYPE) {
    // payload: cond ptr, then the associated mutex ptr.
    void *cobj = NULL, *cmx = NULL;
    memcpy(&cobj, (const void *)mb->cnts, sizeof cobj);
    memcpy(&cmx, (const void *)(mb->cnts + sizeof cobj), sizeof cmx);
    g_rs[r].obj = cobj;
    g_rs[r].cond_mutex = cmx;
  } else if (t == COND_SIGNAL_TYPE || t == COND_BROADCAST_TYPE ||
             t == COND_INIT_TYPE || t == COND_DESTROY_TYPE) {
    void *cobj = NULL;
    memcpy(&cobj, (const void *)mb->cnts, sizeof cobj);
    g_rs[r].obj = cobj;
  }
  SDBG("[inproc_sched] runner %u announced op=%u obj=%p target=%d\n",
       (unsigned)r, t, g_rs[r].obj, (int)g_rs[r].target);
}

static int op_enabled(runner_id_t r) {
  switch (g_rs[r].op) {
    case MUTEX_UNLOCK_TYPE:
    case MUTEX_INIT_TYPE:
    case THREAD_CREATE_TYPE:
    case THREAD_EXIT_TYPE:
      return 1;
    case MUTEX_LOCK_TYPE: {
      int i = mtab_find(g_rs[r].obj);
      return (i < 0) || !g_mtab[i].locked;  // free or not yet seen
    }
    case SEM_POST_TYPE:
    case SEM_INIT_TYPE:
    case SEM_DESTROY_TYPE:
      return 1;
    case SEM_WAIT_TYPE: {
      // Enabled iff the semaphore's count > 0 (matches the model's
      // sem_wait::will_block). An unseen sem (never init'd through the
      // scheduler) has count 0 -> disabled.
      int i = stab_find(g_rs[r].obj);
      return (i >= 0) && g_stab[i].count > 0;
    }
    case COND_ENQUEUE_TYPE:
    case COND_SIGNAL_TYPE:
    case COND_BROADCAST_TYPE:
    case COND_INIT_TYPE:
    case COND_DESTROY_TYPE:
      return 1;
    case COND_WAIT_TYPE: {
      // Enabled only once a signal/broadcast has woken this enqueued waiter AND
      // the associated mutex is free to re-acquire. A never-woken waiter is
      // disabled -> if all runnable threads are so blocked, that's the deadlock
      // (e.g. a lost wakeup: signal fired before the waiter enqueued).
      if (!g_rs[r].cond_woken) return 0;
      int i = mtab_find(g_rs[r].cond_mutex);
      return (i < 0) || !g_mtab[i].locked;
    }
    case THREAD_JOIN_TYPE: {
      runner_id_t tgt = g_rs[r].target;
      return (tgt == RID_INVALID) ||
             (tgt < MAX_TOTAL_THREADS_IN_PROGRAM &&
              g_rs[tgt].status == RS_EXITED);
    }
    default:
      return 1;  // unknown op: don't wedge the scheduler
  }
}

// Release `r`, apply its op's effect, and wait for it to reach its NEXT op
// (or, for THREAD_EXIT, consume the finish-transition wake and mark it exited).
static void step_runner(runner_id_t r) {
  if (g_rs[r].status == RS_READY_START) {
    // First release of a freshly-created thread; it runs to its first real op.
    g_rs[r].status = RS_RUNNING;
    mc_wake_thread(mb_of(r));
    mc_wait_for_thread(mb_of(r));
    read_op(r);
    return;
  }

  uint32_t op = g_rs[r].op;
  switch (op) {
    case MUTEX_LOCK_TYPE:   mtab_set(g_rs[r].obj, 1, r); break;
    case MUTEX_UNLOCK_TYPE: mtab_set(g_rs[r].obj, 0, RID_INVALID); break;
    case MUTEX_INIT_TYPE:   mtab_set(g_rs[r].obj, 0, RID_INVALID); break;
    case SEM_INIT_TYPE:     stab_set(g_rs[r].obj, (int)g_rs[r].target); break;  // target = init count
    case SEM_POST_TYPE:     stab_set(g_rs[r].obj, stab_count(g_rs[r].obj) + 1); break;
    case SEM_WAIT_TYPE:     stab_set(g_rs[r].obj, stab_count(g_rs[r].obj) - 1); break;
    case SEM_DESTROY_TYPE:  break;
    case COND_ENQUEUE_TYPE:
      mtab_set(g_rs[r].cond_mutex, 0, RID_INVALID);  // release the mutex
      g_rs[r].cond_woken = 0;                         // enqueued, not yet woken
      break;
    case COND_WAIT_TYPE:
      mtab_set(g_rs[r].cond_mutex, 1, r);             // re-acquire the mutex
      break;
    case COND_SIGNAL_TYPE:
      // Wake ONE enqueued waiter on this cond (parked on COND_WAIT, same cond,
      // not yet woken); the signal is lost if none are enqueued.
      for (runner_id_t w = 0; w <= g_max_rid; w++) {
        if (g_rs[w].status == RS_PARKED && g_rs[w].op == COND_WAIT_TYPE &&
            g_rs[w].obj == g_rs[r].obj && !g_rs[w].cond_woken) {
          g_rs[w].cond_woken = 1;
          break;
        }
      }
      break;
    case COND_BROADCAST_TYPE:
      for (runner_id_t w = 0; w <= g_max_rid; w++) {
        if (g_rs[w].status == RS_PARKED && g_rs[w].op == COND_WAIT_TYPE &&
            g_rs[w].obj == g_rs[r].obj && !g_rs[w].cond_woken)
          g_rs[w].cond_woken = 1;
      }
      break;
    case COND_INIT_TYPE:
    case COND_DESTROY_TYPE:
      break;
    case THREAD_CREATE_TYPE: {
      // The create has now executed -> the child may start (become schedulable).
      runner_id_t child = g_rs[r].target;
      if (child != RID_INVALID && child < MAX_TOTAL_THREADS_IN_PROGRAM) {
        g_rs[child].status = RS_READY_START;
        if (child > g_max_rid) g_max_rid = child;
      }
      break;
    }
    default: break;  // join: target already exited
  }

  g_rs[r].status = RS_RUNNING;
  mc_wake_thread(mb_of(r));

  if (op == THREAD_EXIT_TYPE) {
    // mc_exit_thread_in_child wakes the scheduler a second time (finish
    // transition) then blocks forever. Consume that wake, mark exited.
    mc_wait_for_thread(mb_of(r));
    g_rs[r].status = RS_EXITED;
    SDBG("[inproc_sched] runner %u EXITED\n", (unsigned)r);
    return;
  }

  mc_wait_for_thread(mb_of(r));
  read_op(r);
}

// b4.3(A): emit the enabled set as a stderr guest event RIGHT BEFORE fuzz_getchar,
// so the per-transition DPOR campaign sees the decision options at this scheduling
// point and can choose the byte (the campaign reads this in the event stream just
// before the SUT parks on fuzz_getchar, waiting_for_input). The per-step enabled
// set is otherwise only visible in the final MCMINI_SCHEDULE_JSON, too late to
// decide on. Distinct prefix from the end-of-run schedule so parsers can tell them
// apart.
static void emit_decide(const runner_id_t *enabled, int ne) {
  static char buf[4096];
  int len = snprintf(buf, sizeof buf,
                     "MCMINI_DECIDE_JSON: {\"step\":%lu,\"enabled\":[", g_steps);
  if (len < 0) return;
  for (int i = 0; i < ne; i++) {
    int rem = (int)sizeof(buf) - len;
    if (rem < 256) break;
    runner_id_t r = enabled[i];
    uint32_t op = pending_op_of(r);
    int n;
    if (op == THREAD_JOIN_TYPE)
      n = snprintf(buf + len, (size_t)rem,
                   "%s{\"runner\":%u,\"op\":\"%s\",\"obj\":\"%p\",\"target\":%u}",
                   i ? "," : "", (unsigned)r, op_name(op), g_rs[r].obj,
                   (unsigned)g_rs[r].target);
    else
      n = snprintf(buf + len, (size_t)rem,
                   "%s{\"runner\":%u,\"op\":\"%s\",\"obj\":\"%p\"}",
                   i ? "," : "", (unsigned)r, op_name(op), g_rs[r].obj);
    if (n > 0 && n < rem) len += n;
  }
  int rem = (int)sizeof(buf) - len;
  if (rem > 4) {
    int n = snprintf(buf + len, (size_t)rem, "]}\n");
    if (n > 0 && n < rem) len += n;
  }
  (void)write(2, buf, (size_t)len);
}

static void *scheduler_main(void *unused) {
  (void)unused;
  // Scheduler is infrastructure, not a runner. Pass ALL pthread interceptions on
  // this thread straight through — most importantly any mutex libvoidstar's
  // fuzz_getchar takes internally (campaign path), which would otherwise hit
  // thread_get_mailbox and assert tid_self!=RID_INVALID (the scheduler never
  // registers). tid_self stays RID_INVALID; mc_in_wrapper makes the wrappers
  // short-circuit to libpthread.
  mc_in_wrapper = 1;
  SDBG("[inproc_sched] scheduler thread started\n");

  // Bootstrap: main (runner 0) runs from the constructor toward its first op.
  g_rs[0].status = RS_RUNNING;
  g_max_rid = 0;
  mc_wait_for_thread(mb_of(0));
  read_op(0);

  for (;;) {
    runner_id_t enabled[MAX_TOTAL_THREADS_IN_PROGRAM];
    int ne = 0, live = 0;
    for (runner_id_t r = 0; r <= g_max_rid; r++) {
      if (g_rs[r].status == RS_READY_START) {
        live++;
        enabled[ne++] = r;  // THREAD_START is always enabled
      } else if (g_rs[r].status == RS_PARKED) {
        live++;
        if (op_enabled(r)) enabled[ne++] = r;
      }
    }

    if (live == 0) {
      SDBG("[inproc_sched] all runners exited -> clean\n");
      sched_emit("clean");
      fflush(stderr);
      _exit(0);
    }
    if (ne == 0) {
      SDBG("[inproc_sched] no enabled runner (%d parked) -> DEADLOCK\n", live);
      build_pending();  // record the blocked ops before emitting
      emit_deadlock_property(1);  // fire the triage property: deadlock reached
      sched_emit("deadlock");
      fflush(stderr);
      _exit(0);
    }

    emit_decide(enabled, ne);  // b4.3(A): publish the decision options BEFORE asking
    int byte = sched_getchar();
    // Register the deadlock property in the Antithesis catalog exactly once, at
    // the first safe point: right after the first fuzz_getchar returns (so
    // libvoidstar is warm — registering earlier, e.g. from a constructor,
    // crashes the SUT). hit=0 lists the property without failing it; the
    // emit_deadlock_property(1) at the deadlock branch flips it to FAILED. This
    // makes the property appear in the triage list even on clean runs.
    static int deadlock_prop_registered = 0;
    if (!deadlock_prop_registered) {
      deadlock_prop_registered = 1;
      emit_deadlock_property(0);
    }
    record_recv(byte);  // diagnostic: exactly what the SUT received this step
    runner_id_t chosen = enabled[((unsigned)byte) % (unsigned)ne];
    uint32_t chosen_op = (g_rs[chosen].status == RS_READY_START)
                             ? OP_THREAD_START_SENTINEL
                             : g_rs[chosen].op;
    // Record the step (step index = g_steps, pre-increment) BEFORE hash_step so
    // the emitted events array is the full interleaving in execution order, each
    // step carrying the enabled set the fuzzer-side DPOR needs.
    record_step(enabled, ne, chosen, chosen_op, g_rs[chosen].obj);
    hash_step(chosen, chosen_op);
    SDBG("[inproc_sched] step %lu: enabled=%d byte=%d -> run runner %u (op=%u)\n",
         g_steps, ne, byte, (unsigned)chosen, chosen_op);
    step_runner(chosen);
  }
  return NULL;
}

void inproc_scheduler_init(void) {
  // Run the scheduler init exactly ONCE per process. libmcmini's constructor
  // (libmcmini_main) is observed firing twice; a second scheduler thread would
  // race the first on the mailbox semaphores and wedge the process. A per-
  // instance `static` flag is NOT enough if the .so is mapped as two instances
  // (each has its own statics), so guard via the process ENVIRONMENT, which is
  // shared across instances. Constructors run sequentially at load time, so this
  // check-then-set is not racy. The pid in the logs disambiguates one-process-
  // twice (same pid) from a fork (different pid) if it recurs.
  if (getenv("MCMINI_SCHED_ACTIVE")) {
    SDBG("[inproc_sched] init SKIPPED — already active (pid=%d)\n", (int)getpid());
    return;
  }
  setenv("MCMINI_SCHED_ACTIVE", "1", 1);

  // Allocate the mailbox array in PROCESS memory (no shm_open): the clean split
  // runs the scheduler as a thread in this same process.
  struct mcmini_shm_file *shm = calloc(1, sizeof *shm);
  if (!shm) {
    fprintf(stderr, "[inproc_sched] calloc(mcmini_shm_file) failed\n");
    _exit(1);
  }
  for (unsigned i = 0; i < MAX_TOTAL_THREADS_IN_PROGRAM; i++)
    mc_runner_mailbox_init(&shm->mailboxes[i]);
  global_shm_start = shm;

  // Register the main thread as runner 0 (runs on the constructor's thread).
  runner_id_t main_rid = mc_register_this_thread();
  (void)main_rid;  // must be 0

  // Spawn the scheduler thread via libpthread directly so it is NOT a runner
  // and never enters the mcmini wrappers.
  pthread_t sched;
  int rc = libpthread_pthread_create(&sched, NULL, &scheduler_main, NULL);
  if (rc != 0) {
    fprintf(stderr, "[inproc_sched] failed to spawn scheduler thread: %d\n", rc);
    _exit(1);
  }
  // Record a clean outcome if main() returns normally (no deadlock). See
  // sched_emit_clean_atexit — this is what makes clean rollouts observable to
  // the strategy, not just deadlocks.
  atexit(&sched_emit_clean_atexit);
  SDBG("[inproc_sched] init done (main=runner %u, pid=%d)\n", (unsigned)main_rid, (int)getpid());
}
