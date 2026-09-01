#define _GNU_SOURCE
#include "mcmini/spy/checkpointing/lockset.h"

#include <dirent.h>    // opendir/readdir for pinning the pre-race checkpoint
#include <fcntl.h>     // open (checkpoint copy)
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>  // mmap for the shadow-entry slab
#include <sys/stat.h>  // stat (newest checkpoint by mtime)
#include <unistd.h>    // _exit

#include "mcmini/spy/intercept/interception.h"  // libpthread_mutex_lock/unlock
#include "mcmini/wrapper_timing.h"              // save_timing_report

/* ------------------------------------------------------------------------- *
 * Configuration / lifecycle
 * ------------------------------------------------------------------------- */

static int ls_enabled = 0;
// Whether the first predicted race terminates Phase 1 (mirrors the deadlock
// detector). On by default; set MCMINI_LOCKSET_KEEP_RUNNING to keep recording
// (e.g. to observe multiple predictions while debugging the predictor).
static int ls_terminate = 1;
// Skip thread-stack / high-mmap addresses when flagging candidates (default on).
// Set MCMINI_LOCKSET_SKIP_STACK=0 to also flag races on stack addresses.
static int ls_skip_stack = 1;
// Pin (snapshot) the newest checkpoint when a surviving candidate fires, so
// Phase 2 can restart just before the race. Default on; MCMINI_LOCKSET_PIN=0 off.
static int ls_pin_enabled = 1;
static pthread_once_t ls_once = PTHREAD_ONCE_INIT;

// Set to true the first time the target calls pthread_create. Accesses before
// any thread is spawned are single-threaded and cannot race; skipping them
// avoids paying stripe-lock overhead during the (often large) init phase.
static atomic_bool ls_parallel_started = ATOMIC_VAR_INIT(false);

void lockset_notify_parallel_start(void) {
  atomic_store_explicit(&ls_parallel_started, true, memory_order_release);
}

static int ls_env_truthy(const char *name) {
  const char *e = getenv(name);
  return (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
}

// Where Phase 1 writes the predicted racing site-id pairs for Phase 2 to read.
static const char *ls_out_path = "mcmini-lockset-races.txt";

static void ls_init_once(void) {
  ls_enabled = ls_env_truthy("MCMINI_LOCKSET");
  ls_terminate = ls_env_truthy("MCMINI_LOCKSET_KEEP_RUNNING") ? 0 : 1;
  // Stack-address filter is on unless explicitly set to "0".
  const char *ss = getenv("MCMINI_LOCKSET_SKIP_STACK");
  ls_skip_stack = (ss != NULL && ss[0] == '0') ? 0 : 1;
  // Checkpoint pinning is on unless explicitly set to "0".
  const char *pn = getenv("MCMINI_LOCKSET_PIN");
  ls_pin_enabled = (pn != NULL && pn[0] == '0') ? 0 : 1;
  const char *o = getenv("MCMINI_LOCKSET_OUT");
  if (o != NULL && o[0] != '\0') ls_out_path = o;
  // Truncate any stale handoff file from a previous run so Phase 2 only sees
  // races predicted by *this* recording.
  if (ls_enabled) {
    FILE *f = fopen(ls_out_path, "w");
    if (f) fclose(f);
  }
}

void lockset_init(void) { pthread_once(&ls_once, ls_init_once); }

int lockset_is_enabled(void) {
  lockset_init();
  return ls_enabled;
}

/* ------------------------------------------------------------------------- *
 * Lock-bit registry: map each lock address to a bit in a 64-bit lockset.
 *
 * Locks beyond the first 64 distinct addresses are not assigned a bit; an
 * access "holding" such a lock simply does not contribute protection, which can
 * only over-report (a false positive), never miss the held set. That is the
 * conservative direction for a predictor.
 * ------------------------------------------------------------------------- */

#define LS_MAX_LOCKS 64

static void *ls_lock_addrs[LS_MAX_LOCKS];
static int ls_lock_count = 0;
static pthread_mutex_t ls_registry_lock = PTHREAD_MUTEX_INITIALIZER;

/// @return bit index in [0, 64) for @p mutex, assigning one on first sight, or
///         -1 if the registry is full.
static int ls_lock_bit(void *mutex) {
  libpthread_mutex_lock(&ls_registry_lock);
  int bit = -1;
  for (int i = 0; i < ls_lock_count; i++) {
    if (ls_lock_addrs[i] == mutex) {
      bit = i;
      break;
    }
  }
  if (bit == -1 && ls_lock_count < LS_MAX_LOCKS) {
    bit = ls_lock_count;
    ls_lock_addrs[ls_lock_count++] = mutex;
  }
  libpthread_mutex_unlock(&ls_registry_lock);
  return bit;
}

/* ------------------------------------------------------------------------- *
 * Per-thread held-lock set (thread-local: no synchronization needed)
 * ------------------------------------------------------------------------- */

static MCMINI_THREAD_LOCAL uint64_t tl_held_locks = 0;

void lockset_acquire(void *mutex) {
  if (!ls_enabled) return;
  int bit = ls_lock_bit(mutex);
  if (bit >= 0) tl_held_locks |= (UINT64_C(1) << bit);
}

void lockset_release(void *mutex) {
  if (!ls_enabled) return;
  int bit = ls_lock_bit(mutex);
  if (bit >= 0) tl_held_locks &= ~(UINT64_C(1) << bit);
}

void lockset_thread_reset(void) { tl_held_locks = 0; }

/* ------------------------------------------------------------------------- *
 * Per-location shadow map + Eraser state machine.
 *
 * Keyed at 8-byte word granularity. A fixed bucket array with chaining; entries
 * are allocated once (on a word's first access). Buckets are guarded by a small
 * array of striped locks so concurrent RECORD-phase threads don't serialize on
 * a single global lock.
 * ------------------------------------------------------------------------- */

typedef enum {
  LS_VIRGIN = 0,
  LS_EXCLUSIVE,
  LS_SHARED,
  LS_SHARED_MODIFIED
} ls_state;

typedef struct ls_entry {
  uintptr_t word;       // key (address & ~7)
  ls_state state;       // Eraser state
  runner_id_t owner;    // exclusive-state owner thread
  uint64_t candidate;   // candidate lockset (valid once SHARED/SHARED_MODIFIED)
  uintptr_t prev_site;  // a representative earlier access (for the report)
  int prev_write;       // whether that earlier access was a write
  runner_id_t prev_thread;
  int already_fired;    // candidate==0 already seen; skip all future accesses
  struct ls_entry *next;
} ls_entry;

#define LS_NBUCKETS 65536u  // power of two (was 4096 — 16× shorter chains)
#define LS_NSTRIPES 1024u   // power of two (was 256 — 4× less contention)

static ls_entry *ls_buckets[LS_NBUCKETS];
static pthread_spinlock_t ls_stripes[LS_NSTRIPES];
static atomic_bool ls_stripes_inited = ATOMIC_VAR_INIT(false);
static pthread_mutex_t ls_stripes_init_lock = PTHREAD_MUTEX_INITIALIZER;

// Shadow entries are bump-allocated from one large MAP_NORESERVE anonymous
// region (a grow-only slab) instead of calloc'd per word. This drops the
// per-entry malloc lock/overhead on the hot path; the unused tail stays
// untouched, so it is free to checkpoint under DMTCP's zero-page skip. Entries
// are never individually freed (matching the previous calloc-never-freed use).
#define LS_POOL_BYTES ((size_t)2 << 30)  // 2 GB virtual reservation (committed lazily)
static char *ls_pool = NULL;             // NULL => fall back to calloc
static atomic_size_t ls_pool_off = ATOMIC_VAR_INIT(0);

static void ls_ensure_stripes(void) {
  if (atomic_load_explicit(&ls_stripes_inited, memory_order_acquire)) return;
  libpthread_mutex_lock(&ls_stripes_init_lock);
  if (!atomic_load_explicit(&ls_stripes_inited, memory_order_relaxed)) {
    for (unsigned i = 0; i < LS_NSTRIPES; i++)
      pthread_spin_init(&ls_stripes[i], PTHREAD_PROCESS_PRIVATE);
    // One grow-only slab for shadow entries. MAP_NORESERVE: only touched pages
    // are committed. On failure ls_pool stays NULL and we fall back to calloc.
    void *p = mmap(NULL, LS_POOL_BYTES, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    ls_pool = (p == MAP_FAILED) ? NULL : (char *)p;
    atomic_store_explicit(&ls_pool_off, 0, memory_order_relaxed);
    atomic_store_explicit(&ls_stripes_inited, true, memory_order_release);
  }
  libpthread_mutex_unlock(&ls_stripes_init_lock);
}

// Bump-allocate one ls_entry from the slab (lock-free); fall back to calloc if
// the slab is absent or exhausted. Anonymous pages are zero on first touch, so
// a slab-allocated entry is already zero-initialized.
static ls_entry *ls_pool_alloc(void) {
  if (ls_pool) {
    size_t off = atomic_fetch_add_explicit(&ls_pool_off, sizeof(ls_entry),
                                           memory_order_relaxed);
    if (off + sizeof(ls_entry) <= LS_POOL_BYTES)
      return (ls_entry *)(ls_pool + off);
    // slab exhausted -> fall through to calloc
  }
  return (ls_entry *)calloc(1, sizeof(ls_entry));
}

static inline unsigned ls_hash(uintptr_t word) {
  // Mix the word (already shifted free of the low 3 bits).
  uintptr_t h = word >> 3;
  h *= UINT64_C(0x9E3779B97F4A7C15);
  return (unsigned)(h >> 40);
}

// Heuristic stack/high-mmap filter. On x86-64 Linux, thread stacks and the mmap
// region sit high (>= ~0x7f0000000000); genuine shared globals/bss/heap sit much
// lower (PIE data ~0x5555..., brk heap). The common Phase-1 false positives are
// producer/consumer handoffs of stack-pointer objects at 0x7fff..., so skipping
// high addresses drops them. CAVEAT: large mmap'd heap buffers also live high, so
// this can miss a real race on such a buffer -- set MCMINI_LOCKSET_SKIP_STACK=0
// to disable. Heuristic, schedule-independent, O(1).
static inline int ls_is_stack_addr(uintptr_t word) {
  return ls_skip_stack && word >= (uintptr_t)0x7f0000000000ULL;
}

/* ------------------------------------------------------------------------- *
 * Race reporting (deduplicated by canonical site-id pair)
 * ------------------------------------------------------------------------- */

static lockset_race_handler ls_handler = NULL;
static pthread_mutex_t ls_report_lock = PTHREAD_MUTEX_INITIALIZER;

#define LS_MAX_REPORTED 4096
static uint64_t ls_reported[LS_MAX_REPORTED];  // packed (min<<32|max) site pair
static int ls_reported_count = 0;

void lockset_set_race_handler(lockset_race_handler handler) {
  ls_handler = handler;
}

static void ls_default_handler(void *addr, uintptr_t site_a, int write_a,
                               runner_id_t thread_a, uintptr_t site_b,
                               int write_b, runner_id_t thread_b) {
  fprintf(stderr,
          "[lockset] PREDICTED DATA RACE @ %p\n"
          "  thread %u: %s (site %lu)\n"
          "  thread %u: %s (site %lu)\n",
          addr, (unsigned)thread_a, write_a ? "write" : "read",
          (unsigned long)site_a, (unsigned)thread_b,
          write_b ? "write" : "read", (unsigned long)site_b);
}

/// Append one predicted race to the Phase-2 handoff file. Each line:
///   RACE <addr> <site_a> <r|w> <site_b> <r|w>
/// Phase 2 (mcmini) loads these site-id pairs to confirm/steer DPOR.
static void ls_append_handoff(void *addr, uintptr_t site_a, int write_a,
                              uintptr_t site_b, int write_b) {
  FILE *f = fopen(ls_out_path, "a");
  if (!f) return;
  fprintf(f, "RACE %p %lu %c %lu %c\n", addr, (unsigned long)site_a,
          write_a ? 'w' : 'r', (unsigned long)site_b, write_b ? 'w' : 'r');
  fclose(f);
}

/* ------------------------------------------------------------------------- *
 * Checkpoint pinning: on a surviving prediction, snapshot the newest periodic
 * checkpoint so Phase 2 can restart from *just before* the race. DMTCP keeps
 * overwriting periodic images, so we copy the current newest aside before it is
 * lost. Copy (not hardlink) so the pin is valid regardless of how DMTCP rewrites
 * the file. Best-effort: if no checkpoint exists yet (race fired before the
 * first interval), we simply skip -- that candidate has no pre-race image.
 * ------------------------------------------------------------------------- */

static atomic_int ls_pin_count = ATOMIC_VAR_INIT(0);

/// @return 1 if a checkpoint was pinned, else 0. @p race_addr is only for the
/// log line, so you can tell which ckpt_race_N goes with which prediction.
static int ls_pin_checkpoint(void *race_addr) {
  if (!ls_pin_enabled) return 0;

  // Find the newest ckpt_*.dmtcp in the checkpoint dir (cwd), excluding our own
  // ckpt_race_* pins, by mtime.
  DIR *d = opendir(".");
  if (!d) return 0;
  char best[512] = {0};
  long best_mtime = -1;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    const char *n = ent->d_name;
    if (strncmp(n, "ckpt_", 5) != 0) continue;
    if (strstr(n, "ckpt_race_") != NULL) continue;  // skip our own pins
    size_t len = strlen(n);
    if (len < 6 || strcmp(n + len - 6, ".dmtcp") != 0) continue;
    struct stat st;
    if (stat(n, &st) != 0) continue;
    if ((long)st.st_mtime > best_mtime) {
      best_mtime = (long)st.st_mtime;
      snprintf(best, sizeof(best), "%s", n);
    }
  }
  closedir(d);
  if (best[0] == '\0') return 0;  // no periodic checkpoint yet

  const int idx = atomic_fetch_add(&ls_pin_count, 1) + 1;
  char dst[512];
  snprintf(dst, sizeof(dst), "ckpt_race_%d.dmtcp", idx);

  int in = open(best, O_RDONLY);
  if (in < 0) return 0;
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (out < 0) {
    close(in);
    return 0;
  }
  char buf[1 << 16];
  ssize_t r;
  int ok = 1;
  while ((r = read(in, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < r) {
      ssize_t w = write(out, buf + off, (size_t)(r - off));
      if (w <= 0) { ok = 0; break; }
      off += w;
    }
    if (!ok) break;
  }
  close(in);
  close(out);
  if (ok)
    fprintf(stderr,
            "[lockset] pinned pre-race checkpoint for race @ %p: %s -> %s\n",
            race_addr, best, dst);
  return ok;
}

/// @return 1 if this (site_a, site_b) pair is newly reported, 0 if a duplicate.
static int ls_report_once(void *addr, uintptr_t site_a, int write_a,
                          runner_id_t thread_a, uintptr_t site_b, int write_b,
                          runner_id_t thread_b) {
  uint32_t lo = (uint32_t)(site_a < site_b ? site_a : site_b);
  uint32_t hi = (uint32_t)(site_a < site_b ? site_b : site_a);
  uint64_t key = ((uint64_t)hi << 32) | lo;

  libpthread_mutex_lock(&ls_report_lock);
  for (int i = 0; i < ls_reported_count; i++) {
    if (ls_reported[i] == key) {
      libpthread_mutex_unlock(&ls_report_lock);
      return 0;
    }
  }
  if (ls_reported_count < LS_MAX_REPORTED) ls_reported[ls_reported_count++] = key;
  libpthread_mutex_unlock(&ls_report_lock);

  lockset_race_handler h = ls_handler ? ls_handler : ls_default_handler;
  h(addr, site_a, write_a, thread_a, site_b, write_b, thread_b);
  return 1;
}

/* ------------------------------------------------------------------------- *
 * Access feed
 * ------------------------------------------------------------------------- */

void lockset_on_access(runner_id_t self, void *addr, size_t size,
                       uintptr_t site_id, int is_write) {
  (void)size;  // v1: keyed by start word only
  if (!ls_enabled) return;
  // Single-threaded init accesses cannot race; skip until first pthread_create.
  if (!atomic_load_explicit(&ls_parallel_started, memory_order_relaxed)) return;
  ls_ensure_stripes();

  const uintptr_t word = (uintptr_t)addr & ~(uintptr_t)7;
  const unsigned h = ls_hash(word);
  const unsigned bidx = h & (LS_NBUCKETS - 1);
  pthread_spinlock_t *stripe = &ls_stripes[h & (LS_NSTRIPES - 1)];

  pthread_spin_lock(stripe);

  ls_entry *e = ls_buckets[bidx];
  while (e && e->word != word) e = e->next;

  // Fast path: once candidate==0 has been seen for this word, the Eraser state
  // machine can only stay in SHARED_MODIFIED with candidate==0 forever. All
  // future accesses are no-ops; skip the state machine entirely.
  if (e && e->already_fired) {
    pthread_spin_unlock(stripe);
    return;
  }

  if (!e) {
    e = ls_pool_alloc();
    if (!e) {  // out of memory: skip silently, predictor is best-effort
      libpthread_mutex_unlock(stripe);
      return;
    }
    e->word = word;
    e->state = LS_VIRGIN;
    e->next = ls_buckets[bidx];
    ls_buckets[bidx] = e;
  }

  int fire = 0;
  uintptr_t rep_prev_site = 0;
  int rep_prev_write = 0;
  runner_id_t rep_prev_thread = 0;

  switch (e->state) {
    case LS_VIRGIN:
      e->state = LS_EXCLUSIVE;
      e->owner = self;
      break;

    case LS_EXCLUSIVE:
      if (self == e->owner) {
        // Still single-threaded: no refinement, no check (init pattern).
        break;
      }
      // A second thread touches the word: begin lockset refinement.
      e->candidate = tl_held_locks;  // == ALL_LOCKS & held(self)
      if (is_write || e->prev_write) {
        e->state = LS_SHARED_MODIFIED;
        if (e->candidate == 0) { fire = 1; e->already_fired = 1; }
      } else {
        e->state = LS_SHARED;
      }
      break;

    case LS_SHARED:
      e->candidate &= tl_held_locks;
      if (is_write) {
        e->state = LS_SHARED_MODIFIED;
        if (e->candidate == 0) { fire = 1; e->already_fired = 1; }
      }
      break;

    case LS_SHARED_MODIFIED:
      e->candidate &= tl_held_locks;
      if (e->candidate == 0) { fire = 1; e->already_fired = 1; }
      break;
  }

  if (fire) {
    rep_prev_site = e->prev_site;
    rep_prev_write = e->prev_write;
    rep_prev_thread = e->prev_thread;

    // Filter raw candidates -> surviving candidates (all O(1), schedule-free;
    // these only drop things that cannot be races, or are very unlikely real
    // shared state). Suppress the *report* only; the Eraser state machine above
    // is left intact.
    //   - same thread: a word touched twice by one thread never races;
    //   - read/read: a race needs at least one write among the two accesses;
    //   - stack/high-mmap address: almost always a producer/consumer handoff FP.
    // Without these, SHARED_MODIFIED re-fires on every later access (incl. reads
    // and same-thread), which is the noise we saw in the handoff.
    if (self == rep_prev_thread ||
        (!is_write && !rep_prev_write) ||
        ls_is_stack_addr(word)) {
      fire = 0;
    }
  }

  // Remember this access as the "earlier" one for a future report.
  e->prev_site = site_id;
  e->prev_write = is_write;
  e->prev_thread = self;

  pthread_spin_unlock(stripe);

  if (fire) {
    int newly =
        ls_report_once((void *)word, rep_prev_site, rep_prev_write,
                       rep_prev_thread, site_id, is_write, self);
    if (newly) {
      ls_append_handoff((void *)word, rep_prev_site, rep_prev_write, site_id,
                        is_write);
      // Snapshot the newest periodic checkpoint (just before this race) so
      // Phase 2 can restart there. One pin per surviving candidate:
      // ckpt_race_1.dmtcp, ckpt_race_2.dmtcp, ...
      ls_pin_checkpoint((void *)word);
    }
    // Stop Phase 1 at the first credible race, the same way the deadlock
    // detector aborts recording when it detects a stall. We use raw _exit(2) so
    // termination bypasses the DMTCP-wrapped atexit/exit path (which otherwise
    // keeps the recording process alive until the next checkpoint interval).
    //
    // NOTE: termination happens *after* the racing access, but ls_pin_checkpoint()
    // above already copied the newest *periodic* checkpoint aside (ckpt_race_N),
    // which was taken before the race -- so Phase 2 has a pre-race image to
    // restart from. (An on-demand checkpoint taken exactly at the flagged region
    // would be tighter; the periodic one is sufficient when DELAY > -i interval.)
    if (newly && ls_terminate) {
      fprintf(stderr,
              "[lockset] terminating Phase 1 (recording) after first "
              "predicted race\n");
      save_timing_report(NULL);
      _exit(0);
    }
  }
}
