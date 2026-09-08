/* imports_helpers.c -- shim bodies that live in the reference ports'
 * imports.c rather than in libc_shim.c.
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds. MIT licensed; see LICENSE.
 * Extracted from sonicjump/source/imports.c and de-Unity-ised.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * tools/gen_imports.py reuses the resolution expression each reference port
 * already uses for a symbol. That is right for the ~120 entries backed by
 * libc_shim.c, and wrong for these: they are defined *inside* the reference
 * imports.c, several of them `static`, so copying the expression alone left
 * 32 dangling references and a tree that compiles file-by-file and then fails
 * to link. Bringing the bodies over is the fix; the audit that caught it is
 * tools/check_links.py.
 *
 * The pthread shims are the important ones and the reason this could not just
 * be pointed at newlib. bionic allocates pthread_mutex_t, pthread_cond_t and
 * pthread_once_t inline in the caller's memory and zero-initialises them;
 * newlib's are a different size and are not zero-initialisable. So the bionic
 * storage is reinterpreted as a pointer slot and lazily backed with a real
 * newlib object. pthread_create_fake matters even more: it is what gives every
 * engine-spawned thread its own bionic TLS block, without which the first
 * stack-protector prologue on that thread reads a canary out of libnx's
 * thread struct. There are 15,198 such prologues in libpapapearsaga.so.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "libc_shim.h"
#include "compat_stubs.h"
#include "imports_helpers.h"
#include "pps_diag.h"
#include "pps_jni.h"   /* jni_quit_requested */

uint64_t __stack_chk_guard_fake = 0x0ull; /* match install_bionic_tls's zeroed tpidr+0x28 slot */
void __stack_chk_fail_fake(void) {  abort(); }

int  __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
void __cxa_finalize_fake(void *dso) { (void)dso; }

// stdin/stdout/stderr point into the fake __sF block (see libc_shim.c)
FILE *stderr_fake = (FILE *)&fake_sF[2];

// ---------------------------------------------------------------------------
// pthread: bionic allocates the opaque types inline and zero-inits them, so we
// lazily back them with heap-allocated newlib objects stashed through the
// caller's pointer slot.
// ---------------------------------------------------------------------------

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (attr && *attr == 1); // bionic PTHREAD_MUTEX_RECURSIVE == 1
  int ret;
  if (recursive) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m; return 0;
}
int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) { pthread_mutex_destroy(*uid); free(*uid); *uid = NULL; }
  return 0;
}
/* Lazily back a bionic static mutex initializer with a real newlib mutex.
 *
 * A real (already-initialized) handle is a heap pointer; anything small is a
 * static initializer left in place by bionic: 0 = normal, 0x4000 = recursive,
 * 0x8000 = errorcheck. All of those need backing.
 *
 * RACE-FREE BY CONSTRUCTION, and it has to be. nativeActivateGame spawns
 * loadWhileShowingSplash on a second thread, and libc++'s shared_ptr atomics
 * (__sp_mut::lock) reach this path from any thread that touches a shared_ptr.
 * The plain read-test-write version this replaces let two threads each
 * allocate a mutex for the same slot; the loser's pointer was overwritten, so
 * the two threads then locked *different* objects believing they were
 * excluding each other. That is silent data corruption, not a crash, which
 * makes it exactly the kind of bug worth removing before it is ever observed.
 *
 * The compare-exchange means only one allocation can win; the loser destroys
 * its own and proceeds with the winner's. */
static int ensure_mutex(pthread_mutex_t **uid) {
  uintptr_t cur = __atomic_load_n((uintptr_t *)uid, __ATOMIC_ACQUIRE);
  if (cur >= 0x10000) return 0;                 /* already a real handle */

  const int recursive = (cur == 0x4000);
  pthread_mutex_t *m = calloc(1, sizeof(*m));
  if (!m) return -1;

  int rc;
  if (recursive) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    rc = pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
  } else {
    rc = pthread_mutex_init(m, NULL);
  }
  if (rc != 0) { free(m); return -1; }

  uintptr_t expected = cur;
  if (!__atomic_compare_exchange_n((uintptr_t *)uid, &expected, (uintptr_t)m,
                                   0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    pthread_mutex_destroy(m);
    free(m);                                    /* another thread won */
  }
  return 0;
}
int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  if (ensure_mutex(uid) < 0) return -1;

  /* Only a CONTENDED lock is a wait worth beaconing. Beaconing every lock
   * would bury the registry in noise from the uncontended common case, and
   * the uncontended path is not where a hang lives. */
  if (pthread_mutex_trylock(*uid) == 0) return 0;

  diag_wait_enter(DIAG_W_MUTEX, *uid);
  const int r = pthread_mutex_lock(*uid);
  diag_wait_exit();
  return r;
}
int pthread_mutex_trylock_fake(pthread_mutex_t **uid) { if (ensure_mutex(uid) < 0) return -1; return pthread_mutex_trylock(*uid); }
int pthread_mutex_unlock_fake(pthread_mutex_t **uid) { if (ensure_mutex(uid) < 0) return -1; return pthread_mutex_unlock(*uid); }
int pthread_mutex_timedlock_fake(pthread_mutex_t **uid, const struct timespec *abs) {
  (void)abs;
  if (ensure_mutex(uid) < 0) return -1;
  for (int i = 0; i < 1000; i++) {
    if (pthread_mutex_trylock(*uid) == 0) return 0;
    svcSleepThread(1000000ull);
  }
  return ETIMEDOUT;
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *attr) {
  (void)attr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) < 0) { free(c); return -1; }
  *cnd = c; return 0;
}
/* Same lazy backing, same race, same fix as ensure_mutex above. A condvar that
 * two threads disagree about is worse than a mutex they disagree about: the
 * signal goes to one object and the wait happens on the other, so the waiter
 * never wakes. */
static int ensure_cond(pthread_cond_t **cnd) {
  uintptr_t cur = __atomic_load_n((uintptr_t *)cnd, __ATOMIC_ACQUIRE);
  if (cur >= 0x10000) return 0;

  pthread_cond_t *c = calloc(1, sizeof(*c));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }

  uintptr_t expected = cur;
  if (!__atomic_compare_exchange_n((uintptr_t *)cnd, &expected, (uintptr_t)c,
                                   0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    pthread_cond_destroy(c);
    free(c);
  }
  return 0;
}
int pthread_cond_broadcast_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1; return pthread_cond_broadcast(*cnd); }
int pthread_cond_signal_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1; return pthread_cond_signal(*cnd); }
int pthread_cond_destroy_fake(pthread_cond_t **cnd) { if (cnd && (uintptr_t)*cnd >= 0x10000) { pthread_cond_destroy(*cnd); free(*cnd); *cnd = NULL; } return 0; }
#define COND_WAIT_CAP_MS 16
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  diag_wait_enter(DIAG_W_COND, cnd);
  /* Blocking wait. If the game parks here waiting for a worker that never
   * signals, the log just stops -- so note the first few and let the
   * heartbeat show whether the count keeps climbing. */

  

  if (ensure_cond(cnd) < 0 || ensure_mutex(mtx) < 0) return -1;
  // Cap the UNTIMED wait too (not just the timed one below). Unity's engine
  // (job system, GfxDevice sync, PreloadManager) blocks the main thread in a
  // plain pthread_cond_wait; a raced/lost pthread_cond_signal -- or a
  // bionic-static-cond object mismatch across signal/wait -- would then hang the
  // whole engine forever (every worker idle, UnityMain parked in
  // svcWaitProcessWideKeyAtomic == the observed frame-2 deadlock). Waking every
  // ~16ms and returning as a spurious wakeup lets the caller re-check its
  // predicate (POSIX-legal: correct waiters always loop on the predicate), which
  // breaks a lost-wakeup stall without changing correct behaviour.
  struct timespec cap;
  clock_gettime(CLOCK_MONOTONIC, &cap);
  long add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  += (cap.tv_nsec + add) / 1000000000L;
  cap.tv_nsec  = (cap.tv_nsec + add) % 1000000000L;
  int r = pthread_cond_timedwait(*cnd, *mtx, &cap);
  { diag_wait_exit(); return (r == ETIMEDOUT) ? 0 : r; }   // timeout -> report as spurious wakeup
}
// Bound every timed cond-wait to at most COND_WAIT_CAP_MS. The .so libs (libc++
// std::condition_variable, and Swappy's frame pacer) compute an ABSOLUTE deadline
// against CLOCK_MONOTONIC, but newlib/libnx's pthread_cond_timedwait may measure
// "now" against a different clock -- a mismatch turns a ~16 ms vsync wait into an
// effectively infinite one (the Swappy hang: engine wedged in condvarWaitTimeout,
// frame counter frozen, black screen). Re-deriving the deadline as
// now(MONOTONIC)+min(requested, CAP) guarantees the wait can't exceed the cap
// regardless of which clock newlib uses, so the pacer hits its timeout fallback
// and keeps pacing. Spurious/early wakeups are POSIX-legal (every correct waiter
// re-checks its predicate), so this is safe in general.
int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (ensure_cond(cnd) < 0 || ensure_mutex(mtx) < 0) return -1;
  struct timespec now, cap;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  = now.tv_sec + (now.tv_nsec + add) / 1000000000L;
  cap.tv_nsec = (now.tv_nsec + add) % 1000000000L;
  // honor the caller's deadline if it's sooner than our cap; else clamp to cap
  const struct timespec *use = &cap;
  if (t && (t->tv_sec < cap.tv_sec ||
            (t->tv_sec == cap.tv_sec && t->tv_nsec <= cap.tv_nsec)))
    use = t;
  int r = pthread_cond_timedwait(*cnd, *mtx, use);
  return r;
}

int pthread_once_fake(volatile int *once, void (*init)(void)) {
  if (!once || !init) return -1;
  if (__sync_lock_test_and_set(once, 1) == 0) (*init)();
  return 0;
}

int pthread_mutexattr_init_fake(int *a) { if (a) *a = 0; return 0; }
int pthread_mutexattr_settype_fake(int *a, int t) { if (a) *a = t; return 0; }

// bionic pthread_attr_t is opaque storage we own; stash size/detach there
#define ATTR_MAGIC 0x41545452 /* 'ATTR' */
typedef struct { uint32_t magic; uint32_t detach; size_t stacksize; } OurAttr;

int pthread_attr_init_fake(void *a) { if (a) { OurAttr *o = a; o->magic = ATTR_MAGIC; o->detach = 0; o->stacksize = 0; } return 0; }
int pthread_attr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_attr_setdetachstate_fake(void *a, int s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->detach = (uint32_t)s; } return 0; }
int pthread_attr_setstacksize_fake(void *a, size_t s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->stacksize = s; } return 0; }
int pthread_attr_getstacksize_fake(const void *a, size_t *s) { if (s) { const OurAttr *o = a; *s = (a && o->magic == ATTR_MAGIC && o->stacksize) ? o->stacksize : (512 * 1024); } return 0; }
int pthread_attr_setschedparam_fake(void *a, const void *p) { (void)a; (void)p; return 0; }

typedef struct { void *(*entry)(void *); void *arg; uint8_t tls[BIONIC_TLS_SIZE]; } ThreadStart;
static void *thread_trampoline(void *p) {
  ThreadStart *ts = (ThreadStart *)p;   /* leaked on purpose: tpidr points into ts->tls */
  install_bionic_tls(ts->tls);
  /* Register here rather than in pthread_create_fake: the registry records
   * the thread's own kernel handle and id, which only exist once it is
   * actually running. This is also the exact point where it becomes capable
   * of running engine code, which is what the watchdog cares about. */
  diag_thread_register(ts->entry, 0);
  void *r = ts->entry(ts->arg);
  diag_thread_unregister();
  return r;
}
int pthread_create_fake(pthread_t *thread, const void *bionic_attr, void *entry, void *arg) {
  /* nativeActivateGame spawns loadWhileShowingSplash here. A thread that never
   * starts and a thread that hangs look identical from the outside, so record
   * the entry point. */
  LOGB("thread: pthread_create entry=%p", entry);

  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  size_t stack = 0;
  if (bionic_attr) {
    const OurAttr *o = bionic_attr;
    if (o->magic == ATTR_MAGIC) stack = o->stacksize;
  }
  if (stack < (2u << 20)) stack = 2u << 20; // 2 MB floor for the heavy engine threads
  pthread_attr_t attr; pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack);
  const int r = pthread_create(thread, &attr, thread_trampoline, ts);
  pthread_attr_destroy(&attr);
  if (r != 0) { free(ts); return r; }
  return 0;
}
int pthread_join_fake(pthread_t thread, void **retval) {
  diag_wait_enter(DIAG_W_JOIN, (const void *)(uintptr_t)thread);
  const int r = pthread_join(thread, retval);
  diag_wait_exit();
  return r;
}
int pthread_setschedparam_fake(pthread_t t, int policy, const void *p) { (void)t; (void)policy; (void)p; return 0; }
int pthread_sigmask_fake(int how, const void *set, void *old) { (void)how; (void)set; (void)old; return 0; }
int pthread_kill_fake(pthread_t t, int sig) { (void)t; (void)sig; return 0; }


// ---------------------------------------------------------------------------
// pthread TLS keys, multiplexed over a single real newlib key.
// devkitA64 backs pthread keys with a tiny pool (~16 libnx TLS slots), but
// Unity's runtime creates dozens during init (46 call sites). The ~17th
// pthread_key_create returns EAGAIN, and libunity treats that as fatal
// (asserts the key was created, else BRK). bionic allows 128 keys; emulate
// that: one real key holds a per-thread value array for up to 128 fake keys.
// ---------------------------------------------------------------------------
#define FAKE_KEYS_MAX 128
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { int used; void (*dtor)(void *); } g_key_table[FAKE_KEYS_MAX];
static pthread_key_t g_master_key;
static int g_master_key_ready;
typedef struct { void *values[FAKE_KEYS_MAX]; } KeyValues;

static void master_key_dtor(void *p) {
  KeyValues *kv = p;
  for (int iter = 0; iter < 4; iter++) {     // POSIX: rerun while dtors set new values
    int again = 0;
    for (int i = 0; i < FAKE_KEYS_MAX; i++) {
      void *v = kv->values[i];
      if (g_key_table[i].used && g_key_table[i].dtor && v) {
        kv->values[i] = NULL;
        g_key_table[i].dtor(v);
        again = 1;
      }
    }
    if (!again) break;
  }
  free(kv);
}

int pthread_key_create_fake(unsigned *key, void (*dtor)(void *)) {
  pthread_mutex_lock(&g_key_mutex);
  if (!g_master_key_ready) {
    if (pthread_key_create(&g_master_key, master_key_dtor) != 0) {
      pthread_mutex_unlock(&g_key_mutex);
      
      return EAGAIN;
    }
    g_master_key_ready = 1;
  }
  for (unsigned i = 0; i < FAKE_KEYS_MAX; i++) {
    if (!g_key_table[i].used) {
      g_key_table[i].used = 1;
      g_key_table[i].dtor = dtor;
      *key = i + 1;                 // 1-based: a zeroed key is invalid
      pthread_mutex_unlock(&g_key_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_key_mutex);
  
  return EAGAIN;
}

int pthread_key_delete_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX) return EINVAL;
  pthread_mutex_lock(&g_key_mutex);
  g_key_table[key - 1].used = 0;
  g_key_table[key - 1].dtor = NULL;
  pthread_mutex_unlock(&g_key_mutex);
  return 0;
}

void *pthread_getspecific_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return NULL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  return kv ? kv->values[key - 1] : NULL;
}

int pthread_setspecific_fake(unsigned key, const void *value) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return EINVAL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  if (!kv) {
    kv = calloc(1, sizeof(*kv));
    if (!kv) return ENOMEM;
    pthread_setspecific(g_master_key, kv);
  }
  kv->values[key - 1] = (void *)value;
  return 0;
}


int z_strcmp(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcmp(a, b);
}
int z_strncmp(const char *a, const char *b, size_t n) {
  if (a == b || n == 0) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strncmp(a, b, n);
}
char *z_strstr(const char *h, const char *n) {
  if (!h || !n) return NULL;
  return strstr(h, n);
}
char *z_strchr(const char *s, int c) { return s ? strchr(s, c) : NULL; }
char *z_strrchr(const char *s, int c) { return s ? strrchr(s, c) : NULL; }
size_t z_strlen(const char *s) { return s ? strlen(s) : 0; }


/* --- small stubs that were static in the reference imports.c --------------- */

int  ret0_i(void) { return 0; }
int  signal_stub(int sig, void *handler) { (void)sig; (void)handler; return 0; }
long sysconf_pass(int name) { return sysconf_fake(name); }

/* sincos is a GNU extension and newlib has neither spelling.
 *
 * sincosf_fake is NOT defined here: libc_shim.c already has one, and defining
 * it in both is a duplicate-symbol link error. Only the double-precision form
 * is missing upstream. Caught by the whole-tree duplicate scan, not by any
 * per-file compile -- both files were individually clean. */
void sincos_fake(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

/* See the note in imports_helpers.h. The engine calls exit() from its console
 * "quit" command and from LaunchResetProgress; both want the normal shutdown
 * path, not process death. */
void pps_exit(int code) {
  (void)code;
  jni_quit_requested = 1;
  for (;;) svcSleepThread(16000000ull);
}
