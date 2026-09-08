/* Bionic-compatible libc wrappers for Android game libraries.
 *
 * Converting wrappers where the bionic and newlib ABIs differ (struct layouts,
 * flag values, missing functions); matching functions pass through from imports.c.
 * Unsupported Android process APIs are represented by compatible stubs.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <switch.h>
#include <EGL/egl.h>     /* eglGetProcAddress: resolve the full GLES API for dlsym */

#include "config.h"
#include "util.h"
#include "error.h"
#include "imports.h"   /* dynlib_find_export (dlsym shim lookup) */
#include "so_util.h"
#include "libc_shim.h"
#include "android_native.h"
#include "sj_paths.h"
#include "sj_trace.h"
#include "fakefd.h"    /* read/write/close/pipe route through the fake-fd layer */
#include "asset_pack.h"
#include "bsd_bridge.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memcpy(dst, src, n); }
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memmove(dst, src, n); }
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) { (void)dstlen; return memset(dst, c, n); }
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcat(dst, src); }
char *__strchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strchr(s, c); }
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcpy(dst, src); }
size_t __strlen_chk_fake(const char *s, size_t slen) { (void)slen; return strlen(s); }
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncat(dst, src, n); }
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncpy(dst, src, n); }
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) { (void)dstlen; (void)srclen; return strncpy(dst, src, n); }
char *__strrchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strrchr(s, c); }
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va); }
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsprintf(s, fmt, va); }

int __snprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va; va_start(va, fmt);
  int r = vsnprintf(s, maxlen, fmt, va);
  va_end(va);
  return r;
}
int __sprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va; va_start(va, fmt);
  int r = vsprintf(s, fmt, va);
  va_end(va);
  return r;
}

// fortified read helpers ignore the buffer-size guard
int   __open_2_fake(const char *path, int flags) { return open_fake(path, flags); }
long  __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) { (void)buflen; return read(fd, buf, count); }
long  __pread_chk_fake(int fd, void *buf, size_t count, long off, size_t buflen) {
  (void)buflen;
  return pread_fake(fd, buf, count, off);
}
void  __FD_SET_chk_fake(int fd, void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) ((unsigned long *)set)[fd / (8 * sizeof(long))] |= (1ul << (fd % (8 * sizeof(long)))); }
int   __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) return (((const unsigned long *)set)[fd / (8 * sizeof(long))] >> (fd % (8 * sizeof(long)))) & 1; return 0; }

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

// android.os.Build.* system properties: hand back Switch-sane values for the keys
// engines query; everything else stays empty (= unset). Returns the value length.
int __system_property_get_fake(const char *name, char *value) {
  if (!value) return 0;
  const char *v = "";
  if (name) {
    if      (!strcmp(name, "ro.build.version.sdk"))        v = "33";
    else if (!strcmp(name, "ro.build.version.release"))    v = "13";
    else if (!strcmp(name, "ro.build.version.codename"))   v = "REL";
    else if (!strcmp(name, "ro.product.cpu.abi"))          v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist"))      v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist64"))    v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abi2"))         v = "";
    else if (!strcmp(name, "ro.product.model"))            v = "Switch";
    else if (!strcmp(name, "ro.product.manufacturer"))     v = "Nintendo";
    else if (!strcmp(name, "ro.product.brand"))            v = "Nintendo";
    else if (!strcmp(name, "ro.product.name"))             v = "Switch";
    else if (!strcmp(name, "ro.product.device"))           v = "Switch";
    else if (!strcmp(name, "ro.product.board"))            v = "nx";
    else if (!strcmp(name, "ro.hardware"))                 v = "nx";
    else if (!strcmp(name, "ro.board.platform"))           v = "nx";
    else if (!strcmp(name, "ro.build.fingerprint"))        v = "Nintendo/Switch/Switch:13/REL/10007:user/release-keys";
    else if (!strcmp(name, "ro.build.characteristics"))    v = "default";
    else if (!strcmp(name, "ro.build.type"))               v = "user";
    else if (!strcmp(name, "ro.build.tags"))               v = "release-keys";
    else if (!strcmp(name, "ro.debuggable"))               v = "0";
    else if (!strcmp(name, "ro.secure"))                   v = "1";
    else if (!strcmp(name, "ro.kernel.qemu"))              v = "0";
    else if (!strcmp(name, "ro.opengles.version"))         v = "196610"; /* GLES 3.2 */
    else if (!strcmp(name, "dalvik.vm.heapsize"))          v = "512m";
    else if (!strcmp(name, "persist.sys.timezone"))        v = "UTC";
  }
  size_t n = strlen(v);
  if (n > 91) n = 91;            /* PROP_VALUE_MAX-1 */
  memcpy(value, v, n); value[n] = '\0';
  return (int)n;
}
/* Unity 6 / Swappy read properties -- crucially "ro.build.version.sdk", the gate that
 * decides whether to use the NDK AChoreographer -- via the 2-step find+read API, NOT
 * __system_property_get. Unshimmed, Swappy sees API 0 (<24), never touches
 * AChoreographer, gets no vsync source, and the frame loop wedges after a few frames.
 * Route them through the same property table so Swappy sees API 33 and drives frames
 * via AChoreographer (fed by our fake-choreographer driver in imports.c). */
const void *__system_property_find_fake(const char *name) {
  if (!name) return NULL;
  char buf[96]; buf[0] = '\0';
  __system_property_get_fake(name, buf);
  return buf[0] ? (const void *)name : NULL;   /* handle == name; NULL if we have no value */
}
int __system_property_read_fake(const void *pi, char *name, char *value) {
  const char *n = (const char *)pi;
  if (name)  { if (n) { size_t k = strlen(n); if (k > 31) k = 31; memcpy(name, n, k); name[k] = '\0'; } else name[0] = '\0'; }
  char buf[96]; buf[0] = '\0';
  if (n) __system_property_get_fake(n, buf);
  if (value) strcpy(value, buf);
  return (int)strlen(buf);
}
unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int gettid_fake(void) {
  u64 tid = 1;
  if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE)) && tid)
    return (int)(tid & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID            178
#define ARM64_SYS_FUTEX             98
#define ARM64_SYS_SCHED_SETAFFINITY 122
#define ARM64_SYS_PROCESS_VM_READV  270
#define ARM64_SYS_PROCESS_VM_WRITEV 271

// futex(2) emulation over libnx mutex+condvar (il2cpp's GC/thread-pool/locks use
// raw futex). Wait queues are hashed by uaddr into buckets; FUTEX_WAKE wakes the
// whole bucket (waiters re-check *uaddr, so over-broad wakes are harmless).
// Infinite waits are capped at 16ms and return as if woken (a missed wake is
// recovered by the re-poll since the waiter re-checks *uaddr).
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CMD_MASK    0x7f  // strip FUTEX_PRIVATE_FLAG(128)/CLOCK_REALTIME(256)
#define FUTEX_BUCKETS     256

static Mutex   futex_lock[FUTEX_BUCKETS];   // libnx Mutex/CondVar are u32; 0 == ready
static CondVar futex_cond[FUTEX_BUCKETS];

static long futex_impl(volatile int32_t *uaddr, int op, int val, const struct timespec *to) {
  const int cmd = op & FUTEX_CMD_MASK;
  const unsigned h = (unsigned)(((uintptr_t)uaddr >> 4) & (FUTEX_BUCKETS - 1));
  if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
    long ret = 0;
    mutexLock(&futex_lock[h]);
    if (*uaddr != val) {
      errno = EAGAIN; ret = -1;
    } else if (to) {
      const u64 ns = (u64)to->tv_sec * 1000000000ULL + (u64)to->tv_nsec;
      if (R_FAILED(condvarWaitTimeout(&futex_cond[h], &futex_lock[h], ns))) {
        errno = ETIMEDOUT; ret = -1;
      }
    } else {
      condvarWaitTimeout(&futex_cond[h], &futex_lock[h], 16000000ULL); // capped infinite wait
    }
    mutexUnlock(&futex_lock[h]);
    return ret;
  }
  if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
    mutexLock(&futex_lock[h]);
    condvarWakeAll(&futex_cond[h]);
    mutexUnlock(&futex_lock[h]);
    return val > 0 ? val : 0; // approximate count woken
  }
  errno = ENOSYS;
  return -1;
}

/* newlib has no <sys/uio.h>; the kernel iovec layout is just {ptr, len}. */
struct nx_iovec { void *iov_base; size_t iov_len; };

/* Validate that [addr, addr+len) is mapped and readable via svcQueryMemory, so a
 * self process_vm_readv can copy safely instead of risking a fault. */
static int nx_addr_readable(uintptr_t addr, size_t len) {
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == 0) return 0;                 /* MemType_Unmapped */
    if ((mi.perm & Perm_R) == 0) return 0;      /* not readable */
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if (be <= a) return 0;
    a = be;
  }
  return 1;
}

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID: return gettid_fake();
    case ARM64_SYS_FUTEX: {
      va_list va; va_start(va, number);
      volatile int32_t *uaddr = va_arg(va, volatile int32_t *);
      const int op  = va_arg(va, int);
      const int val = va_arg(va, int);
      const struct timespec *to = va_arg(va, const struct timespec *);
      va_end(va);
      return futex_impl(uaddr, op, val, to);
    }
    case ARM64_SYS_SCHED_SETAFFINITY:
      return 0; // affinity hints are advisory; pretend success
    case ARM64_SYS_PROCESS_VM_READV:
    case ARM64_SYS_PROCESS_VM_WRITEV: {
      /* Validate each remote range before copying. */
      va_list va; va_start(va, number);
      long pid                   = va_arg(va, long); (void)pid;
      const struct nx_iovec *liov   = va_arg(va, const struct nx_iovec *);
      unsigned long lcnt         = va_arg(va, unsigned long);
      const struct nx_iovec *riov   = va_arg(va, const struct nx_iovec *);
      unsigned long rcnt         = va_arg(va, unsigned long);
      va_end(va);
      int writing = (number == ARM64_SYS_PROCESS_VM_WRITEV);
      ssize_t total = 0;
      unsigned long li = 0, ri = 0; size_t lo = 0, ro = 0;
      while (li < lcnt && ri < rcnt) {
        char *lp = (char *)liov[li].iov_base + lo;
        char *rp = (char *)riov[ri].iov_base + ro;
        size_t lrem = liov[li].iov_len - lo, rrem = riov[ri].iov_len - ro;
        size_t n = lrem < rrem ? lrem : rrem;
        char *read_side = writing ? lp : rp;
        if (!nx_addr_readable((uintptr_t)read_side, n)) {
          if (total == 0) { errno = EFAULT; return -1; }
          return total;
        }
        if (writing) memcpy(rp, lp, n); else memcpy(lp, rp, n);
        total += (ssize_t)n; lo += n; ro += n;
        if (lo == liov[li].iov_len) { li++; lo = 0; }
        if (ro == riov[ri].iov_len) { ri++; ro = 0; }
      }
      return total;
    }
  }
  
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
int sched_get_priority_max_fake(int policy) { (void)policy; return 0; }
int sched_get_priority_min_fake(int policy) { (void)policy; return 0; }
void android_set_abort_message_fake(const char *msg) {
  /* bionic keeps this for the crash reporter: it is the text of a fatal
   * assert, the most informative string the engine ever produces. It was
   * being discarded along with everything else the game logged. */
  if (msg) printf("[F/abort] %s\n", msg);
}
size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
int __register_atfork_fake(void) { return 0; }
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    // Report 512 MB (matches synthetic /proc/meminfo MemTotal) to make Unity's
    // DynamicHeap reserve fewer 256MB regions; real backing (arena/OC) holds more.
    case BIONIC_SC_PHYS_PAGES: return (512ll * 1024 * 1024) / 0x1000;
    default: return -1;
  }
}
long pathconf_fake(const char *path, int name) { (void)path; (void)name; return -1; }

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

// Android's StreamingAssets path is rooted at "/assets", while the Switch payload
// is rooted at the process cwd as "assets/". Resolve that exact prefix first. Asset
// packs are also addressed as "<packdir>/<file>.mvgl" but shipped flat, so retain
// the older basename/Data fallback after it. Reads only, and only when the target
// actually exists, so save paths and unrelated absolute paths cannot be redirected.
static int basename_fallback(const char *path, char *out, size_t outsz) {
  struct stat st;
  if (!strncmp(path, "/assets/", 8)) {
    snprintf(out, outsz, "assets/%s", path + 8);
    if (stat(out, &st) == 0) return 1;
  }
  const char *slash = strrchr(path, '/');
  if (!slash || !slash[1]) return 0;   // no subdir component to strip
  snprintf(out, outsz, "%s", slash + 1); // basename, resolved against the cwd
  if (stat(out, &st) == 0) return 1;
  // Loose Play-Asset-Delivery streaming side-files (resources.resource, *.resS) are
  // referenced by bare name but live in assets/bin/Data/ -- try there too.
  snprintf(out, outsz, "assets/bin/Data/%s", slash + 1);
  return stat(out, &st) == 0;
}

// Create one directory, refusing paths newlib's mkdir() mishandles: a bare
// "device:" path null-derefs newlib's devoptab (Data Abort at mkdir_r +0x68).
static int safe_mkdir(const char *p) {
  if (!p || !*p) { errno = EINVAL; return -1; }
  const char *colon = strchr(p, ':');
  if (colon) {                       // has a "device:" prefix
    const char *in = colon + 1;      // the path inside the device
    while (*in == '/') in++;
    if (!*in) { errno = EEXIST; return 0; }  // "sdmc:" / "sdmc:/" -> root, skip
    // A single top-level component ("sdmc:/switch") also null-derefs the devoptab.
    if (!strchr(in, '/')) { errno = EEXIST; return 0; }
  }
  return mkdir(p, 0777);
}

// mkdir -p: create `dir` and every missing parent. Begin the walk *after* GAME_HOME:
// the game root and its ancestors already exist, and a top-level mkdir() null-derefs
// newlib's devoptab (Data Abort at mkdir_r +0x68).
static void mkdir_p_dir(const char *dir) {
  if (!dir || !*dir) return;
  char tmp[512];
  if (snprintf(tmp, sizeof(tmp), "%s", dir) <= 0) return;
  size_t skip;
  const size_t glen = strlen(sj_home());
  if (strncmp(tmp, sj_home(), glen) == 0 && (tmp[glen] == '/' || tmp[glen] == '\0')) {
    skip = glen;                                  // only create *under* the game root
  } else {
    const char *colon = strchr(tmp, ':');         // unknown base: at least skip "device:"
    skip = colon ? (size_t)(colon + 1 - tmp) : 0;
  }
  for (char *p = tmp + skip + 1; *p; p++)
    if (*p == '/') { *p = '\0'; safe_mkdir(tmp); *p = '/'; }
  if (tmp[skip]) safe_mkdir(tmp);
}
// create the parent directory chain of a file path
static void mkdir_parents(const char *filepath) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", filepath);
  char *last = strrchr(tmp, '/');
  if (!last || last == tmp) return;
  *last = '\0';
  mkdir_p_dir(tmp);
}

// mkdir wrapper: create the full chain and treat "already exists" as success
int mkdir_fake(const char *path, unsigned mode) {
  (void)mode;
  if (!path || !*path) { errno = EINVAL; return -1; }
  mkdir_p_dir(path);
  int r = safe_mkdir(path);
  if (r != 0 && errno == EEXIST) r = 0;
  return r;
}

/* ---- read-ahead cache for big archive files (data.unity3d, sharedassets) ----
 * Unity deserializes archives with thousands of tiny read()s; with no page cache
 * on Switch each is a direct SD access and boot crawls. Keep aligned 1 MiB pages
 * and virtualize the file position. A single moving window is not enough for the
 * merged data.unity3d. Give only very large archives a 64-page LRU;
 * ordinary resource files retain one page and therefore their old memory cost.
 * Keyed by fd; the real fd position is used only as our scratch. */
#define RA_SLOTS       8
#define RA_PAGE        (1u << 20)  /* 1 MiB, also the required alignment */
#define RA_MAX_PAGES   64
#define RA_LARGE_MIN   (128L << 20)
static struct RaCache {
  int  fd;           /* -1 == free */
  long pos;          /* virtual file position (what read/lseek observe) */
  long size;         /* file size (for SEEK_END) */
  long base[RA_MAX_PAGES]; /* aligned file offset of each cached page */
  long len[RA_MAX_PAGES];  /* valid bytes in each page; zero == unused */
  uint64_t used[RA_MAX_PAGES]; /* LRU clock */
  uint64_t clock;
  unsigned page_count;
  size_t buf_bytes;
  unsigned char *buf;
} g_ra[RA_SLOTS] = {
  { .fd = -1 }, { .fd = -1 }, { .fd = -1 }, { .fd = -1 },
  { .fd = -1 }, { .fd = -1 }, { .fd = -1 }, { .fd = -1 },
};
static Mutex g_ra_lock;
/* newlib/libnx has no positional-read primitive available to the Android
 * imports, so pread is implemented with lseek+read. Serialize every such raw
 * file-position transaction, including read-ahead refills, or concurrent Unity
 * AssetBundle workers can seek the shared descriptor out from under each other. */
static Mutex g_positional_io_lock;
static struct RaCache *ra_find(int fd) {
  if (fd < 0) return NULL;
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].fd == fd) return &g_ra[i];
  return NULL;
}
void ra_attach(int fd, long size) {
  mutexLock(&g_ra_lock);
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].fd < 0) {
    unsigned pages = size >= RA_LARGE_MIN ? RA_MAX_PAGES : 1;
    size_t wanted = (size_t)pages * RA_PAGE;
    if (g_ra[i].buf_bytes < wanted) {
      unsigned char *grown = realloc(g_ra[i].buf, wanted);
      if (grown) {
        g_ra[i].buf = grown;
        g_ra[i].buf_bytes = wanted;
      } else {
        /* A cache allocation must never make the file unavailable. If the 64 MiB
         * hot cache cannot be reserved, retain/fall back to the original 1 MiB. */
        pages = 1;
        wanted = RA_PAGE;
        if (g_ra[i].buf_bytes < wanted) {
          grown = realloc(g_ra[i].buf, wanted);
          if (grown) {
            g_ra[i].buf = grown;
            g_ra[i].buf_bytes = wanted;
          }
        }
      }
    }
    if (g_ra[i].buf) {
      g_ra[i].fd = fd; g_ra[i].pos = 0; g_ra[i].size = size;
      g_ra[i].page_count = pages; g_ra[i].clock = 0;
      for (unsigned p = 0; p < RA_MAX_PAGES; p++) {
        g_ra[i].base[p] = 0; g_ra[i].len[p] = 0; g_ra[i].used[p] = 0;
      }
      
    }
    break;
  }
  mutexUnlock(&g_ra_lock);
}
void ra_detach(int fd) {
  mutexLock(&g_ra_lock);
  struct RaCache *c = ra_find(fd);
  if (c) c->fd = -1;   /* keep buf allocated for reuse */
  mutexUnlock(&g_ra_lock);
}
static long ra_read(struct RaCache *c, int fd, void *buf, size_t count) {
  size_t done = 0;
  mutexLock(&g_ra_lock);
  while (done < count) {
    if (c->pos < 0 || c->pos >= c->size) break;

    int page = -1;
    for (unsigned p = 0; p < c->page_count; p++) {
      if (c->len[p] > 0 && c->pos >= c->base[p] &&
          c->pos < c->base[p] + c->len[p]) {
        page = (int)p;
        break;
      }
    }

    if (page < 0) {
      page = 0;
      for (unsigned p = 0; p < c->page_count; p++) {
        if (c->len[p] == 0) { page = (int)p; break; }
        if (c->used[p] < c->used[page]) page = (int)p;
      }
      long page_base = c->pos & ~((long)RA_PAGE - 1);
      long wanted = c->size - page_base;
      if (wanted > (long)RA_PAGE) wanted = (long)RA_PAGE;
      unsigned char *page_buf = c->buf + (size_t)page * RA_PAGE;
      mutexLock(&g_positional_io_lock);
      if (lseek(fd, page_base, SEEK_SET) < 0) {
        mutexUnlock(&g_positional_io_lock);
        break;
      }
      long r = 0;
      while (r < wanted) {
        long k = read(fd, page_buf + r, (size_t)(wanted - r));
        if (k <= 0) break;
        r += k;
      }
      mutexUnlock(&g_positional_io_lock);
      if (r <= 0) break;
      c->base[page] = page_base; c->len[page] = r;
    }
    c->used[page] = ++c->clock;
    long avail = (c->base[page] + c->len[page]) - c->pos;
    if (avail <= 0) break;
    size_t n = (count - done < (size_t)avail) ? count - done : (size_t)avail;
    memcpy((char *)buf + done,
           c->buf + (size_t)page * RA_PAGE + (c->pos - c->base[page]), n);
    c->pos += n; done += n;
  }
  mutexUnlock(&g_ra_lock);
  return (long)done;
}

/* lseek for arm64: off_t is already 64-bit, so this also services lseek64
 * (which the archive reader uses to size data.unity3d via SEEK_END). */
long z_lseek(int fd, long off, int whence) {
  if (asset_pack_fd_is(fd)) return asset_pack_lseek_fd(fd, off, whence);
  struct RaCache *c = ra_find(fd);
  if (c) {   /* virtualized position -- don't touch the real fd here */
    mutexLock(&g_ra_lock);
    long np = (whence == SEEK_SET) ? off : (whence == SEEK_CUR) ? c->pos + off : c->size + off;
    c->pos = np;
    mutexUnlock(&g_ra_lock);
    return np;
  }
  return lseek(fd, off, whence);
}

static const char *synthetic_proc(const char *path);  /* defined below */

// Serve /proc and /sys reads that arrive through raw open() (e.g. /proc/self/maps).
// newlib's open() can't be memory-backed, so materialize the synthetic content into
// a small file and hand back a real fd. Returns -1 if `path` isn't synthesized.
static int synth_proc_open(const char *path) {
  if (!path) return -1;
  if (strncmp(path, "/proc/", 6) && strncmp(path, "/sys/", 5)) return -1;
  static char buf[16384];
  int len;
  if (!strcmp(path, "/proc/self/maps") || !strcmp(path, "/proc/self/smaps")) {
    len = so_dump_maps(buf, sizeof buf);
  } else {
    const char *s = synthetic_proc(path);
    if (!s) return -1;                                   // not /proc or /sys
    len = (int)strlen(s);
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    memcpy(buf, s, (size_t)len);
  }
  char safe[160]; size_t j = 0;
  for (const char *p = path; *p && j < sizeof safe - 1; p++) safe[j++] = (*p == '/') ? '_' : *p;
  safe[j] = '\0';
  char tf[256];
  snprintf(tf, sizeof tf, "%s/.synth%s", sj_home(), safe);
  int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (wfd >= 0) { if (write(wfd, buf, (size_t)len) < 0) { /* best effort */ } close(wfd); }
  return open(tf, O_RDONLY);
}

// ---------------------------------------------------------------------------
// Synthetic inode numbers. libnx's fsdev returns st_ino==0 for every file, but
// il2cpp's System.IO share layer keys its open-file table on (st_dev,st_ino), so
// unrelated files collide and Easy Save throws "Sharing violation" every frame.
// Give each distinct path a stable non-zero inode (only when the real one is 0).
// fstat() has no path, so a small fd->inode map is filled at open() time.
#define FD_INO_MAX 4096
static uint64_t g_fd_ino[FD_INO_MAX];
static int cache_path_is(const char *path);
static uint64_t path_ino(const char *path) {
  uint64_t h = 1469598103934665603ULL;               // FNV-1a 64 offset basis
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
  return h ? h : 1;                                   // 0 means "no inode" -- avoid it
}
static void fd_ino_set(int fd, const char *path) { if (fd >= 0 && fd < FD_INO_MAX) g_fd_ino[fd] = path_ino(path); }
static void fd_ino_clear(int fd) { if (fd >= 0 && fd < FD_INO_MAX) g_fd_ino[fd] = 0; }

int truncate_fake(const char *path, long len) {
  if (!path || len < 0) {
    errno = EINVAL;
    return -1;
  }

  /* Unity's cache writer keeps the destination open, then reserves its archive
   * header with truncate(path, size). fsdev does not allow the same file to be
   * opened a second time for writing, so implement POSIX truncate semantics by
   * resizing the matching live descriptor when one exists. */
  const uint64_t ino = path_ino(path);
  for (int fd = 0; fd < FD_INO_MAX; fd++) {
    if (__atomic_load_n(&g_fd_ino[fd], __ATOMIC_RELAXED) != ino)
      continue;
    const int result = ftruncate(fd, (off_t)len);
    if (result == 0)
      return 0;
  }

  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  const int result = ftruncate(fd, (off_t)len);
  const int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  return result;
}

/* il2cpp copies the entire libil2cpp.so (~176MB) into il2cpp/il2cpp.usym/ for Sentry
 * crash-symbol upload -- unneeded for the port and a huge SD write that stalls boot for
 * tens of seconds. Discard writes to any *.usym* path: the dest is still created (0-byte,
 * so close/fstat work) but the 176MB write is no-op'd. The .usym is never read back by
 * the running game (only uploaded to Sentry on a crash, which we don't do). */
#define MAX_SINK_FDS 8
static int   g_sink_fds[MAX_SINK_FDS];
static Mutex g_sink_lock;   /* libnx Mutex is zero-init safe */
int  usym_sink_is(int fd) {
  if (fd < 0) return 0;
  mutexLock(&g_sink_lock);
  for (int i = 0; i < MAX_SINK_FDS; i++) if (g_sink_fds[i] == fd) { mutexUnlock(&g_sink_lock); return 1; }
  mutexUnlock(&g_sink_lock); return 0;
}
static void usym_sink_add(int fd) {
  mutexLock(&g_sink_lock);
  for (int i = 0; i < MAX_SINK_FDS; i++) if (!g_sink_fds[i] || g_sink_fds[i] < 0) { g_sink_fds[i] = fd; break; }
  mutexUnlock(&g_sink_lock);
}
void usym_sink_del(int fd) {
  mutexLock(&g_sink_lock);
  for (int i = 0; i < MAX_SINK_FDS; i++) if (g_sink_fds[i] == fd) { g_sink_fds[i] = -1; break; }
  mutexUnlock(&g_sink_lock);
}

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  const int cvt = convert_open_flags(flags);
  const int writing = (flags & 3) != 0 || (flags & LINUX_O_CREAT);
  if (!writing) {
    // /dev/urandom + /dev/random: Switch has no /dev node, but Mono/.NET and asset
    // crypto open these. Materialize real CSPRNG bytes (randomGet) into a file and
    // hand back a real fd so read() just works.
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) {
      static char rbuf[65536];
      randomGet(rbuf, sizeof rbuf);
      char tf[256];
      snprintf(tf, sizeof tf, "%s/.synth_dev_random", sj_home());
      int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (wfd >= 0) { if (write(wfd, rbuf, sizeof rbuf) < 0) { /* best effort */ } close(wfd); }
      int rfd = open(tf, O_RDONLY);
      fd_ino_set(rfd, path);
      
      return rfd;
    }
    // synthetic /proc, /sys (incl. self/maps)
    int sfd = synth_proc_open(path);
    if (sfd >= 0) { fd_ino_set(sfd, path);  return sfd; }
    int packed_fd = asset_pack_open_path(path);
    if (packed_fd >= 0) {
      fd_ino_set(packed_fd, path);
      return packed_fd;
    }
  }
  const char *actual_path = path;
  int fd = open(actual_path, cvt, mode);
  if (fd < 0 && writing) {
    // save files: the target subdir may not exist yet -- create it and retry
    mkdir_parents(path);
    fd = open(actual_path, cvt, mode);
  }
  if (fd < 0 && (flags & 3) == 0 && !(flags & LINUX_O_CREAT)) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt)))
      fd = open(alt, cvt, mode);
  }
  if (fd >= 0) {
    fd_ino_set(fd, path);
    if (writing && strstr(path, ".usym")) {   /* Sentry symbol dump -> discard sink */
      usym_sink_add(fd);
      
      return fd;
    }
    struct stat _st;
    if (fstat(fd, &_st) == 0) {
      
      /* Big read-only asset files (data.unity3d ~424MB, sharedassets*.resource)
       * get a read-ahead cache so Unity's tiny per-field reads hit RAM, not SD. */
      if (!writing && _st.st_size >= (4 << 20))
        ra_attach(fd, (long)_st.st_size);
    }
  }
  return fd;
}
int openat_fake(int dirfd, const char *path, int flags, ...) {
  (void)dirfd;
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  // Delegate to open_fake so /dev/urandom, synthetic /proc + /sys, save-dir
  // creation and basename fallback all apply (some libc paths route open->openat).
  return open_fake(path, flags, mode);
}
int unlinkat_fake(int dirfd, const char *path, int flags) { (void)dirfd; (void)flags; return unlink(path); }

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
  uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t __pad1;
  int64_t st_size; int32_t st_blksize; int32_t __pad2; int64_t st_blocks;
  struct bionic_timespec st_atim; struct bionic_timespec st_mtim; struct bionic_timespec st_ctim;
  uint32_t __unused4; uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino; out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size; out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime; out->st_mtim.tv_sec = in->st_mtime; out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  uint64_t packed_size, packed_ino;
  int packed_directory;
  if (asset_pack_stat_path_info(path, &packed_size, &packed_ino,
                                &packed_directory)) {
    memset(st, 0, sizeof(*st));
    st->st_ino = packed_ino;
    st->st_mode = packed_directory ? S_IFDIR | 0555 : S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = (int64_t)packed_size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)((packed_size + 511) / 512);
    return 0;
  }

  /* fsdev's path-based stat can keep reporting the size from open time until
   * the writer closes its handle. Unity validates a cache archive before it
   * closes that handle, so obtain the live size from the matching descriptor. */
  if (cache_path_is(path)) {
    const uint64_t ino = path_ino(path);
    for (int fd = 0; fd < FD_INO_MAX; fd++) {
      if (__atomic_load_n(&g_fd_ino[fd], __ATOMIC_RELAXED) != ino)
        continue;
      struct stat live;
      if (fstat(fd, &live) != 0)
        continue;
      mutexLock(&g_positional_io_lock);
      const long current = lseek(fd, 0, SEEK_CUR);
      const long end = lseek(fd, 0, SEEK_END);
      if (current >= 0)
        lseek(fd, current, SEEK_SET);
      mutexUnlock(&g_positional_io_lock);
      if (end >= 0)
        live.st_size = (off_t)end;
      convert_stat(&live, st);
      if (st->st_ino == 0)
        st->st_ino = ino;
      return 0;
    }
  }

  const char *actual_path = path;
  struct stat real; int r = stat(actual_path, &real);
  if (r != 0) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt))) r = stat(alt, &real);
  }
  if (r == 0) {
    convert_stat(&real, st);
    if (st->st_ino == 0) st->st_ino = path_ino(path);   // fsdev gives 0 -> synth
  }
  return r;
}
int fstat_fake(int fd, struct bionic_stat *st) {
  uint64_t packed_size, packed_ino;
  int packed_directory;
  if (asset_pack_fstat_fd(fd, &packed_size, &packed_ino, &packed_directory)) {
    memset(st, 0, sizeof(*st));
    st->st_ino = packed_ino;
    st->st_mode = packed_directory ? S_IFDIR | 0555 : S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = (int64_t)packed_size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)((packed_size + 511) / 512);
    return 0;
  }
  struct stat real; const int r = fstat(fd, &real);
  if (r == 0) {
    convert_stat(&real, st);
    if (st->st_ino == 0) {                               // mirror stat(path)'s inode
      uint64_t ino = (fd >= 0 && fd < FD_INO_MAX) ? g_fd_ino[fd] : 0;
      st->st_ino = ino ? ino : ((uint64_t)(fd + 1) * 2654435761ULL) | 1;
    }
  }
  return r;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256];
};

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // not thread-safe (matches bionic readdir)
  memset(&out, 0, sizeof(out));
  out.d_reclen = sizeof(out);
  if (asset_pack_dir_is(dirp)) {
    const char *name = asset_pack_readdir_path(dirp, &out.d_type, &out.d_ino);
    if (!name) return NULL;
    snprintf(out.d_name, sizeof(out.d_name), "%s", name);
  } else {
    struct dirent *e = readdir((DIR *)dirp);
    if (!e) return NULL;
    out.d_ino = e->d_ino;
    out.d_type = e->d_type;
    snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  }
  return &out;
}

int closedir_fake(void *dirp) {
  return asset_pack_dir_is(dirp) ? asset_pack_closedir_path(dirp)
                                 : closedir((DIR *)dirp);
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C-locale versions
// ---------------------------------------------------------------------------

void *newlocale_fake(int mask, const char *locale, void *base) { (void)mask; (void)locale; (void)base; return (void *)1; }
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha) WRAP_ISW_L(iswblank) WRAP_ISW_L(iswcntrl) WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower) WRAP_ISW_L(iswprint) WRAP_ISW_L(iswpunct) WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper) WRAP_ISW_L(iswxdigit) WRAP_ISW_L(towlower) WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) { (void)loc; return strftime(s, max, fmt, (const struct tm *)tm); }
long double strtold_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) { if (dst) dst[i] = (unsigned char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) { if (dst) dst[i] = (char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

// --- anonymous mmap arena (page-granular; supports sub-range munmap) ----------
// Switch has no mmap, and Unity reserves big aligned pools by over-mmapping then
// munmapping the unaligned head/tail. A plain malloc/free-per-mmap would free the
// whole block when the head is trimmed and corrupt the kept middle, so we manage a
// dedicated aligned arena (carved in __libnx_initheap) with a per-page used-bitmap:
// mmap finds a free page run; munmap clears exactly the sub-range's pages. Big
// requests use the patched Unity granule alignment so Unity only trims the tail.
// ------------------------------------------------------------------------------
extern void  *g_mmap_arena_base;   // set by __libnx_initheap (main.c)
extern size_t g_mmap_arena_size;
extern size_t g_mmap_big_align;
extern int    g_overcommit;        // 1 = alias-region on-demand commit
extern u64    g_alias_base, g_alias_size;

#define BIONIC_MAP_ANONYMOUS 0x20
#define BIONIC_MAP_SHARED    0x01
#define MMAP_PAGE       0x1000u
#define MMAP_BIG_ALIGN  g_mmap_big_align
#define MMAP_BIG_THRESH MMAP_BIG_ALIGN
#define BIONIC_PROT_NONE 0x0
#define BIONIC_PROT_WRITE 0x2
#define BIONIC_MADV_DONTNEED 4

static uint8_t *mmap_arena;    // granule-aligned usable base (published last)
static size_t   mmap_usable;   // usable bytes
static size_t   mmap_pages;    // usable / page
static uint8_t *mmap_used;     // 1 byte/page bitmap: reserved (address space)
static uint8_t *mmap_committed;// 1 byte/page bitmap: physically committed (overcommit only)
static size_t   g_committed_pages;   // running count of committed pages
static Mutex    g_mmap_lock;   // zero-init == valid unlocked libnx mutex

// --- overcommit commit/decommit (caller holds g_mmap_lock) -------------------
// svcMapPhysicalMemory zero-fills and draws from the freed physical limit; it
// FAILS on already-mapped pages, so we only ever commit pages we track as
// uncommitted, in contiguous runs. Out-of-physical is logged, not fatal.
static void arena_commit_locked(size_t first, size_t cnt) {
  size_t i = 0;
  while (i < cnt) {
    if (mmap_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && !mmap_committed[first + i + run]) run++;
    u64 a = (u64)(uintptr_t)(mmap_arena + (first + i) * MMAP_PAGE);
    Result rc = svcMapPhysicalMemory((void *)a, (u64)run * MMAP_PAGE);
    if (R_SUCCEEDED(rc)) {
      for (size_t k = 0; k < run; k++) mmap_committed[first + i + k] = 1;
      g_committed_pages += run;
    }
    i += run ? run : 1;
  }
}

static void arena_decommit_locked(size_t first, size_t cnt) {
  size_t i = 0;
  while (i < cnt) {
    if (!mmap_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && mmap_committed[first + i + run]) run++;
    u64 a = (u64)(uintptr_t)(mmap_arena + (first + i) * MMAP_PAGE);
    if (R_SUCCEEDED(svcUnmapPhysicalMemory((void *)a, (u64)run * MMAP_PAGE))) {
      for (size_t k = 0; k < run; k++) mmap_committed[first + i + k] = 0;
      g_committed_pages -= run;
    }
    i += run ? run : 1;
  }
}

// Translate a range to clamped pages inside the arena.
static int arena_page_range(void *addr, size_t len, size_t *first, size_t *cnt) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return 0;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return 0;
  size_t f = off / MMAP_PAGE;
  size_t c = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (f + c > mmap_pages) c = mmap_pages - f;
  *first = f; *cnt = c;
  return 1;
}

static void arena_commit_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) arena_commit_locked(first, cnt);
  mutexUnlock(&g_mmap_lock);
}

static void arena_decommit_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) arena_decommit_locked(first, cnt);
  mutexUnlock(&g_mmap_lock);
}

static void arena_dontneed_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) {
    for (size_t i = 0; i < cnt; ) {
      if (!mmap_committed[first + i]) { i++; continue; }
      size_t run = 0;
      while (i + run < cnt && mmap_committed[first + i + run]) run++;
      memset(mmap_arena + (first + i) * MMAP_PAGE, 0, run * MMAP_PAGE);
      i += run;
    }
  }
  mutexUnlock(&g_mmap_lock);
}

/* Stack-region overcommit arena. */
#define OC_NOSRC 0xFFFFFFFFu
static uint8_t  *oc_base;
static size_t    oc_pages;
static uint8_t  *oc_used;
static uint8_t  *oc_committed;
static uint32_t *oc_srcpg;
static uint8_t  *oc_pool;
static size_t    oc_pool_pages;
static size_t    oc_pool_bump;
static uint32_t *oc_pool_next;
static uint32_t  oc_pool_freehead = OC_NOSRC;
static size_t    oc_live_pages;

int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes) {
  if (!window || !pool || !window_bytes || !pool_bytes) return 0;
  size_t wp = window_bytes / MMAP_PAGE, pp = pool_bytes / MMAP_PAGE;
  uint8_t  *u = (uint8_t *)calloc(wp, 1);
  uint8_t  *c = (uint8_t *)calloc(wp, 1);
  uint32_t *s = (uint32_t *)malloc(wp * sizeof *s);
  uint32_t *n = (uint32_t *)malloc(pp * sizeof *n);
  if (!u || !c || !s || !n) { free(u); free(c); free(s); free(n); return 0; }
  memset(s, 0xFF, wp * sizeof *s);
  mutexLock(&g_mmap_lock);
  oc_base = (uint8_t *)window; oc_pages = wp; oc_used = u; oc_committed = c;
  oc_srcpg = s; oc_pool = (uint8_t *)pool; oc_pool_pages = pp;
  oc_pool_bump = 0; oc_pool_next = n; oc_pool_freehead = OC_NOSRC; oc_live_pages = 0;
  mutexUnlock(&g_mmap_lock);
  return 1;
}

static int oc_contains(void *addr) {
  return oc_pages && (uint8_t *)addr >= oc_base &&
         (uint8_t *)addr < oc_base + oc_pages * MMAP_PAGE;
}

static int oc_range_occupied(size_t i, size_t need) {
  uint64_t a = (uint64_t)(uintptr_t)(oc_base + i * MMAP_PAGE);
  uint64_t end = a + (uint64_t)need * MMAP_PAGE;
  int occupied = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) { occupied = 1; break; }
    uint64_t span_end = mi.addr + mi.size;
    if (span_end <= a) { occupied = 1; break; }
    if (mi.type != MemType_Unmapped) {
      occupied = 1;
      uint64_t start = mi.addr > (uint64_t)(uintptr_t)oc_base
          ? mi.addr : (uint64_t)(uintptr_t)oc_base;
      size_t p0 = (size_t)((start - (uint64_t)(uintptr_t)oc_base) / MMAP_PAGE);
      size_t p1 = (size_t)((span_end - (uint64_t)(uintptr_t)oc_base + MMAP_PAGE - 1) / MMAP_PAGE);
      for (size_t k = p0; k < p1 && k < oc_pages; k++) oc_used[k] = 1;
    }
    a = span_end;
  }
  return occupied;
}

static void *oc_alloc_locked(size_t len, size_t *got) {
  *got = 0;
  if (!oc_pages) return NULL;
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (!need) need = 1;
  const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;
  size_t kept = need > step ? need - step : need;
  for (size_t i = 0; i + need <= oc_pages; i += step) {
    size_t run = 0;
    while (run < need && !oc_used[i + run]) run++;
    if (run == need) {
      if (oc_range_occupied(i, need)) continue;
      for (size_t k = 0; k < need; k++) oc_used[i + k] = 1;
      *got = need * MMAP_PAGE;
      return oc_base + i * MMAP_PAGE;
    }
  }
  for (size_t i = 0; i < oc_pages; i += step) {
    if (i + need <= oc_pages) continue;
    size_t avail = oc_pages - i;
    if (avail < kept) continue;
    size_t run = 0;
    while (run < avail && !oc_used[i + run]) run++;
    if (run == avail) {
      if (oc_range_occupied(i, avail)) continue;
      for (size_t k = 0; k < avail; k++) oc_used[i + k] = 1;
      *got = avail * MMAP_PAGE;
      return oc_base + i * MMAP_PAGE;
    }
  }
  return NULL;
}

static void oc_commit_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  size_t i = 0;
  while (i < cnt) {
    if (oc_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && !oc_committed[first + i + run]) run++;
    size_t done = 0;
    while (done < run) {
      size_t chunk, src0;
      int from_bump = oc_pool_bump < oc_pool_pages;
      if (from_bump) {
        chunk = run - done;
        if (oc_pool_bump + chunk > oc_pool_pages) chunk = oc_pool_pages - oc_pool_bump;
        src0 = oc_pool_bump;
      } else if (oc_pool_freehead != OC_NOSRC) {
        chunk = 1; src0 = oc_pool_freehead;
        while (chunk < run - done && src0 + chunk < oc_pool_pages &&
               oc_pool_next[src0 + chunk - 1] == src0 + chunk)
          chunk++;
      } else {
        fatal_error("Out of memory: the game exhausted the %u MB commit pool.",
                    (unsigned)((oc_pool_pages * MMAP_PAGE) >> 20));
      }
      void *dst = oc_base + (first + i + done) * MMAP_PAGE;
      void *src = oc_pool + src0 * MMAP_PAGE;
      Result rc = svcMapMemory(dst, src, (u64)chunk * MMAP_PAGE);
      if (R_FAILED(rc)) {
        fatal_error("Could not back the game heap at %p (rc=0x%x).", dst, rc);
      }
      if (from_bump) oc_pool_bump += chunk;
      else           oc_pool_freehead = oc_pool_next[src0 + chunk - 1];
      memset(dst, 0, chunk * MMAP_PAGE);   // committed anon must read as zero
      for (size_t k = 0; k < chunk; k++) {
        oc_committed[first + i + done + k] = 1;
        oc_srcpg[first + i + done + k] = (uint32_t)(src0 + k);
      }
      oc_live_pages += chunk; done += chunk;
    }
    i += run;
  }
}

// Decommit fully-covered pages of [addr,len): un-alias them (the pool source becomes
// accessible again) and push the pool pages onto the free list for reuse. Contents
// are not preserved -- a later mprotect(RW) recommits zeroed pages, which is the
// anon-decommit contract. caller holds g_mmap_lock.
static void oc_decommit_locked(void *addr, size_t len) {
  if (!oc_pages || (uint8_t *)addr < oc_base) return;
  size_t off   = (size_t)((uint8_t *)addr - oc_base);
  size_t first = (off + MMAP_PAGE - 1) / MMAP_PAGE;        // partial head page stays
  size_t lastx = (off + len) / MMAP_PAGE;                  // partial tail page stays
  if (lastx > oc_pages) lastx = oc_pages;
  size_t i = first;
  while (i < lastx) {
    if (!oc_committed[i]) { i++; continue; }
    size_t run = 1;                                        // batch contiguous dst+src
    while (i + run < lastx && oc_committed[i + run] &&
           oc_srcpg[i + run] == oc_srcpg[i] + run) run++;
    void *dst = oc_base + i * MMAP_PAGE;
    void *src = oc_pool + (size_t)oc_srcpg[i] * MMAP_PAGE;
    if (R_SUCCEEDED(svcUnmapMemory(dst, src, (u64)run * MMAP_PAGE))) {
      /* Push in reverse so the free-list links this contiguous source run in
       * ascending order; oc_commit_locked can then batch it into one SVC. */
      for (size_t k = run; k-- > 0; ) {
        uint32_t s = oc_srcpg[i + k];
        oc_pool_next[s] = oc_pool_freehead; oc_pool_freehead = s;
        oc_committed[i + k] = 0; oc_srcpg[i + k] = OC_NOSRC;
      }
      oc_live_pages -= run;
    }
    i += run;
  }
}

/* Our fake mprotect cannot enforce PROT_NONE. Keep OC aliases mapped so a kernel
 * TLS/stack allocation cannot steal the reservation before Unity recommits it,
 * but clear the covered committed pages to preserve anonymous-decommit semantics.
 * caller holds g_mmap_lock. */
static void oc_discard_locked(void *addr, size_t len) {
  if (!oc_pages || (uint8_t *)addr < oc_base || !len) return;
  size_t off = (size_t)((uint8_t *)addr - oc_base);
  size_t first = off / MMAP_PAGE;
  size_t cnt = (len + (off & (MMAP_PAGE - 1)) + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  size_t i = 0;
  while (i < cnt) {
    if (!oc_committed[first + i]) { i++; continue; }
    size_t run = 1;
    while (i + run < cnt && oc_committed[first + i + run]) run++;
    memset(oc_base + (first + i) * MMAP_PAGE, 0, run * MMAP_PAGE);
    i += run;
  }
}

// munmap of an OC range: decommit whatever was committed (returning pool pages),
// then release the reservation.
static void oc_free_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  oc_decommit_locked(addr, cnt * MMAP_PAGE);
  for (size_t i = 0; i < cnt; i++)
    if (!oc_committed[first + i]) oc_used[first + i] = 0;
}

// caller holds g_mmap_lock
static void mmap_arena_init_locked(void) {
  if (mmap_arena) return;
  uint8_t *base; size_t usable;
  if (g_mmap_arena_base) {
    base   = (uint8_t *)g_mmap_arena_base;   // dedicated, already granule-aligned
    usable = g_mmap_arena_size;
  } else {
    // fallback (small heap / applet): memalign a modest arena (< 2GB newlib limit)
    const size_t want = (size_t)768 * 1024 * 1024 + MMAP_BIG_ALIGN;
    uint8_t *raw = memalign(MMAP_PAGE, want);
    if (!raw) fatal_error("mmap arena alloc (%u MB) failed", (unsigned)(want >> 20));
    base   = (uint8_t *)(((uintptr_t)raw + (MMAP_BIG_ALIGN - 1)) & ~(MMAP_BIG_ALIGN - 1));
    usable = (size_t)768 * 1024 * 1024;
  }
  size_t pages  = usable / MMAP_PAGE;
  uint8_t *used = (uint8_t *)calloc(pages, 1);
  if (!used) fatal_error("mmap bitmap alloc failed");
  if (g_overcommit) {
    mmap_committed = (uint8_t *)calloc(pages, 1);
    if (!mmap_committed) fatal_error("mmap commit-bitmap alloc failed");
  }
  mmap_usable = usable; mmap_pages = pages; mmap_used = used;
  mmap_arena  = base;   // publish last (alloc/free key off this)
  
}

// caller holds g_mmap_lock.
// Returns the mapped base and writes the bytes actually reserved (in-arena) to *got.
// Big alignment over-maps reserve the whole run and let the tail-munmap give it back;
// but the last slot's over-map runs past the arena end, so there we reserve only
// [slot, arena_end) and Unity's tail-munmap beyond the arena is a harmless no-op.
static void *mmap_arena_alloc_locked(size_t len, size_t *got) {
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (!need) need = 1;
  if (len >= MMAP_BIG_THRESH) {
    const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;
    size_t kept = need > step ? need - step : need;   // pages Unity actually keeps
    // pass 1: full over-map fits within the arena (normal case for all but the last slot)
    for (size_t i = 0; i + need <= mmap_pages; i += step) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
    }
    // pass 2: tail slot -- the over-map would spill past the arena end, but the kept
    // block fits in [slot, arena_end). Reserve only that; the spill is trimmed away.
    for (size_t i = 0; i < mmap_pages; i += step) {
      if (i + need <= mmap_pages) continue;          // handled by pass 1
      size_t avail = mmap_pages - i;
      if (avail < kept) continue;                    // kept block wouldn't fit
      size_t run = 0;
      while (run < avail && !mmap_used[i + run]) run++;
      if (run == avail) {
        for (size_t k = 0; k < avail; k++) mmap_used[i + k] = 1;
        *got = avail * MMAP_PAGE;                     // only the in-arena portion
        return mmap_arena + i * MMAP_PAGE;
      }
    }
  } else {
    for (size_t i = 0; i + need <= mmap_pages; ) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
      i += run + 1;
    }
  }
  *got = 0;
  return NULL;
}

static void mmap_arena_free(void *addr, size_t len) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return;
  size_t first = off / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  mutexLock(&g_mmap_lock);
  for (size_t k = 0; k < cnt && first + k < mmap_pages; k++)
    mmap_used[first + k] = 0;
  mutexUnlock(&g_mmap_lock);
}

// Stopgap: when the arena is exhausted, small il2cpp/GC mmaps are served from
// newlib's free heap via memalign, tracked so munmap can free them (consumes
// physical newlib heap -- not real overcommit).
#define MMAP_FALLBACK_MAX 4096
static struct { void *ptr; size_t len; } g_fb[MMAP_FALLBACK_MAX];
static int   g_fb_n = 0;
static Mutex g_fb_lock;

static void *mmap_fallback(size_t length, int flags, int fd, long offset) {
  /* Big anon reservations are Unity Dynamic-Heap pools whose allocator masks
   * pointers to a large-aligned base for block indices, so give them a
   * MMAP_BIG_ALIGN-aligned base (a 4KB-aligned one faults at libunity+0xdce75c). */
  size_t align = (length >= MMAP_BIG_THRESH && (flags & BIONIC_MAP_ANONYMOUS))
                   ? MMAP_BIG_ALIGN : MMAP_PAGE;
  void *q = memalign(align, length);
  if (!q) return NULL;
  long got = 0;
  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(q, 0, length);
  } else {
    if (fd >= 0) {
      if (asset_pack_fd_is(fd)) {
        got = asset_pack_pread_fd(fd, q, length, offset);
        if (got < 0) got = 0;
      } else {
        long cur = lseek(fd, 0, SEEK_CUR);
        if (lseek(fd, offset, SEEK_SET) >= 0)
          while ((size_t)got < length) { long r = read(fd, (char *)q + got, length - got); if (r <= 0) break; got += r; }
        if (cur >= 0) lseek(fd, cur, SEEK_SET);
      }
    }
    if ((size_t)got < length) memset((char *)q + got, 0, length - got);
  }
  mutexLock(&g_fb_lock);
  if (g_fb_n >= MMAP_FALLBACK_MAX) {
    mutexUnlock(&g_fb_lock);
    free(q);
    
    errno = ENOMEM;
    return NULL;
  }
  g_fb[g_fb_n].ptr = q;
  g_fb[g_fb_n].len = length;
  g_fb_n++;
  mutexUnlock(&g_fb_lock);
  
  return q;
}

// returns 1 and frees if addr was a fallback allocation
static int mmap_fallback_free(void *addr) {
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++) {
    if (g_fb[i].ptr == addr) {
      size_t released = g_fb[i].len;
      free(addr);
      g_fb[i] = g_fb[--g_fb_n];
      mutexUnlock(&g_fb_lock);
      (void)released;
      return 1;
    }
  }
  mutexUnlock(&g_fb_lock);
  return 0;
}

// ---- read-only file-map dedup cache ---------------------------------------
// il2cpp builds a stack trace for every thrown managed exception and the symbolizer
// mmaps libil2cpp.so + libunity.so read-only each time, never munmapping -> newlib
// exhausts and self-exits. Dedup: one shared pinned buffer per (inode, offset, len).
#define MAPC_N 24
static struct { uint64_t ino; long off; size_t len; void *ptr; } g_mapc[MAPC_N];
static int g_mapc_n = 0;
static void *mapcache_get(uint64_t ino, long off, size_t len) {
  void *r = NULL;
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_mapc_n; i++)
    if (g_mapc[i].ino == ino && g_mapc[i].off == off && g_mapc[i].len == len) { r = g_mapc[i].ptr; break; }
  mutexUnlock(&g_fb_lock);
  return r;
}
static void mapcache_put(uint64_t ino, long off, size_t len, void *ptr) {
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++)          // pin: drop from fallback free-list
    if (g_fb[i].ptr == ptr) { g_fb[i] = g_fb[--g_fb_n]; break; }
  if (g_mapc_n < MAPC_N) { g_mapc[g_mapc_n].ino = ino; g_mapc[g_mapc_n].off = off;
                           g_mapc[g_mapc_n].len = len; g_mapc[g_mapc_n].ptr = ptr; g_mapc_n++; }
  mutexUnlock(&g_fb_lock);
}

/* Retain writable shared mappings so msync can flush them to SD storage. */
#define WRITABLE_FILE_MAPS 128
typedef struct {
  uint8_t *base;
  size_t length;
  int fd;
  long offset;
} WritableFileMap;

static WritableFileMap g_writable_maps[WRITABLE_FILE_MAPS];
static Mutex g_writable_map_lock;

static int write_mapping_at(int fd, const void *data, size_t length, long offset) {
  size_t written = 0;
  int ok = 1;
  mutexLock(&g_positional_io_lock);
  long current = lseek(fd, 0, SEEK_CUR);
  if (lseek(fd, offset, SEEK_SET) < 0) {
    ok = 0;
  } else {
    while (written < length) {
      long put = write(fd, (const uint8_t *)data + written, length - written);
      if (put <= 0) { ok = 0; break; }
      written += (size_t)put;
    }
  }
  if (current >= 0) lseek(fd, current, SEEK_SET);
  if (ok && fsync(fd) != 0) ok = 0;
  mutexUnlock(&g_positional_io_lock);
  return ok;
}

static int writable_map_add(void *base, size_t length, int prot, int flags,
                            int fd, long offset) {
  if (!base || fd < 0 || !(prot & BIONIC_PROT_WRITE) ||
      (flags & 3) != BIONIC_MAP_SHARED)
    return 1;
  int retained = dup(fd);
  if (retained < 0) return 0;
  mutexLock(&g_writable_map_lock);
  int slot = -1;
  for (int i = 0; i < WRITABLE_FILE_MAPS; i++) {
    if (!g_writable_maps[i].base) { slot = i; break; }
  }
  if (slot >= 0) {
    g_writable_maps[slot].base = base;
    g_writable_maps[slot].length = length;
    g_writable_maps[slot].fd = retained;
    g_writable_maps[slot].offset = offset;
  }
  mutexUnlock(&g_writable_map_lock);
  if (slot < 0) {
    close(retained);
    return 0;
  }
  return 1;
}

static int writable_map_flush(void *address, size_t length, int remove) {
  uintptr_t requested_start = (uintptr_t)address;
  uintptr_t requested_end = length > UINTPTR_MAX - requested_start
                              ? UINTPTR_MAX : requested_start + length;
  int ok = 1;
  mutexLock(&g_writable_map_lock);
  for (int i = 0; i < WRITABLE_FILE_MAPS; i++) {
    WritableFileMap *map = &g_writable_maps[i];
    if (!map->base) continue;
    uintptr_t map_start = (uintptr_t)map->base;
    uintptr_t map_end = map_start + map->length;
    uintptr_t start = requested_start > map_start ? requested_start : map_start;
    uintptr_t end = requested_end < map_end ? requested_end : map_end;
    if (start >= end) continue;

    size_t span = (size_t)(end - start);
    long file_offset = map->offset + (long)(start - map_start);
    if (!write_mapping_at(map->fd, (const void *)start, span, file_offset)) {
      ok = 0;
    }

    if (!remove) continue;
    if (start == map_start && end == map_end) {
      close(map->fd);
      memset(map, 0, sizeof(*map));
      continue;
    }
    if (start == map_start) {
      size_t removed = (size_t)(end - map_start);
      map->base += removed;
      map->offset += (long)removed;
      map->length -= removed;
      continue;
    }
    if (end == map_end) {
      map->length = (size_t)(start - map_start);
      continue;
    }

    int split = -1;
    for (int j = 0; j < WRITABLE_FILE_MAPS; j++) {
      if (!g_writable_maps[j].base) { split = j; break; }
    }
    int split_fd = split >= 0 ? dup(map->fd) : -1;
    if (split_fd >= 0) {
      g_writable_maps[split].base = (uint8_t *)end;
      g_writable_maps[split].length = (size_t)(map_end - end);
      g_writable_maps[split].fd = split_fd;
      g_writable_maps[split].offset = map->offset + (long)(end - map_start);
      map->length = (size_t)(start - map_start);
    } else {
      close(map->fd);
      memset(map, 0, sizeof(*map));
    }
  }
  mutexUnlock(&g_writable_map_lock);
  return ok ? 0 : -1;
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset) {
  (void)addr;
  if (length == 0) length = 1;

  // Big anonymous PROT_NONE reservations -> stack-region OC arena. Back the whole
  // returned reservation before exposing its address to Unity; otherwise a kernel
  // TLS page can land in an unbacked portion before a later mprotect commit.
  if (oc_pages && length >= MMAP_BIG_THRESH &&
      (flags & BIONIC_MAP_ANONYMOUS) && prot == BIONIC_PROT_NONE) {
    size_t ocres = 0;
    mutexLock(&g_mmap_lock);
    void *op = oc_alloc_locked(length, &ocres);
    if (op) oc_commit_locked(op, ocres);
    mutexUnlock(&g_mmap_lock);
    if (op) {
      
      return op;
    }
    
  }

  size_t reserved = 0;
  mutexLock(&g_mmap_lock);
  mmap_arena_init_locked();
  void *p = mmap_arena_alloc_locked(length, &reserved);
  mutexUnlock(&g_mmap_lock);
  /* A file-backed map must be fully readable; a tail-overflow reservation (reserved
   * < length) would silently truncate the file in RAM. Hand those to newlib, which
   * backs the whole length. */
  if (p && !(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0 && reserved < length) {
    mutexLock(&g_mmap_lock);
    mmap_arena_free(p, length);
    mutexUnlock(&g_mmap_lock);
    
    p = NULL;
  }
  if (!p) {
    // Arena exhausted: route to newlib's free heap regardless of size (il2cpp's
    // resource-extraction maps can exceed 64MB, and rejecting them NULL-derefs the
    // engine). Read-only file maps get deduped (see above).
    int ro_file = fd >= 0 && !(flags & BIONIC_MAP_ANONYMOUS) && !(prot & BIONIC_PROT_WRITE);
    uint64_t mino = 0;
    if (ro_file) {
      uint64_t packed_size;
      if (!asset_pack_fstat_fd(fd, &packed_size, &mino, NULL) &&
          fd < FD_INO_MAX)
        mino = g_fd_ino[fd];
    }
    if (mino) { void *hit = mapcache_get(mino, offset, length); if (hit) return hit; }
    size_t fallback_length = length;
    
    if ((flags & BIONIC_MAP_ANONYMOUS) && prot == BIONIC_PROT_NONE &&
        length > MMAP_BIG_ALIGN &&
        (length & (MMAP_BIG_ALIGN - 1)) == MMAP_BIG_ALIGN - MMAP_PAGE) {
      fallback_length = length - (MMAP_BIG_ALIGN - MMAP_PAGE);
    }
    void *q = mmap_fallback(fallback_length, flags, fd, offset);
    if (q) {
      if (mino) mapcache_put(mino, offset, length, q);
      if (!writable_map_add(q, length, prot, flags, fd, offset)) {
        mmap_fallback_free(q);
        errno = ENOMEM;
        return (void *)-1;
      }
      return q;
    }
    
    errno = ENOMEM; return (void *)-1;
  }

  // Never touch beyond what we actually reserved in-arena (tail over-maps reserve
  // less than the requested length; the spill lives past the arena and is trimmed).
  size_t fill = length < reserved ? length : reserved;

  if (g_overcommit) {
    // PROT_NONE reservation: address space only, no physical -- the whole point.
    // The engine commits the sub-ranges it uses later via mprotect(RW).
    if (prot == BIONIC_PROT_NONE) return p;
    // Otherwise commit now (anon RW, file maps): svcMapPhysicalMemory zero-fills,
    // so anon needs no memset; file maps read their contents over the zeros.
    arena_commit_range(p, fill);
    if (!(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0) {
      long got = 0;
      if (asset_pack_fd_is(fd)) {
        got = asset_pack_pread_fd(fd, p, fill, offset);
        if (got < 0) got = 0;
      } else {
        long cur = lseek(fd, 0, SEEK_CUR);
        if (lseek(fd, offset, SEEK_SET) >= 0)
          while ((size_t)got < fill) { long r = read(fd, (char *)p + got, fill - got); if (r <= 0) break; got += r; }
        if (cur >= 0) lseek(fd, cur, SEEK_SET);
      }
    }
    if (!writable_map_add(p, fill, prot, flags, fd, offset)) {
      mmap_arena_free(p, fill);
      errno = ENOMEM;
      return (void *)-1;
    }
    return p;
  }

  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(p, 0, fill);   // anonymous memory must read back as zero
  } else {
    // File-backed mapping: pull [offset, offset+fill) into RAM (no real mmap).
    long got = 0;
    if (fd >= 0) {
      if (asset_pack_fd_is(fd)) {
        got = asset_pack_pread_fd(fd, p, fill, offset);
        if (got < 0) got = 0;
      } else {
        long cur = lseek(fd, 0, SEEK_CUR);
        if (lseek(fd, offset, SEEK_SET) >= 0) {
          while ((size_t)got < fill) {
            long r = read(fd, (char *)p + got, fill - (size_t)got);
            if (r <= 0) break;
            got += r;
          }
        }
        if (cur >= 0) lseek(fd, cur, SEEK_SET);
      }
    }
    if ((size_t)got < fill) memset((char *)p + got, 0, fill - (size_t)got);
  }
  if (!writable_map_add(p, fill, prot, flags, fd, offset)) {
    mmap_arena_free(p, fill);
    errno = ENOMEM;
    return (void *)-1;
  }
  return p;
}

int munmap_fake(void *addr, size_t length) {
  if (writable_map_flush(addr, length, 1) != 0) return -1;
  if (mmap_fallback_free(addr)) return 0;   // newlib fallback allocation
  if (oc_contains(addr)) {                   // stack-region OC reservation
    mutexLock(&g_mmap_lock);
    oc_free_locked(addr, length);
    mutexUnlock(&g_mmap_lock);
    return 0;
  }
  arena_decommit_range(addr, length);       // reclaim physical (overcommit only)
  mmap_arena_free(addr, length);            // unreserve address space
  return 0;
}

int msync_fake(void *addr, size_t length, int flags) {
  (void)flags;
  return writable_map_flush(addr, length, 0);
}

// In overcommit mode mprotect drives commit/decommit: RW/R commits physical at
// the alias address, PROT_NONE decommits it (safe -- reuse re-mprotects to RW).
// In heap-backed mode the arena is always RW so this is a no-op.
int mprotect_fake(void *addr, size_t len, int prot) {
  if (oc_contains(addr)) {
    // OC reservation: backing was installed before mmap returned. Retain it across
    // PROT_NONE so kernel TLS cannot steal a page; munmap performs the actual reclaim.
    mutexLock(&g_mmap_lock);
    if (prot == BIONIC_PROT_NONE) oc_discard_locked(addr, len);
    else                          oc_commit_locked(addr, len);
    mutexUnlock(&g_mmap_lock);
    return 0;
  }
  if (!g_overcommit) return 0;
  if (prot == BIONIC_PROT_NONE) arena_decommit_range(addr, len);
  else                          arena_commit_range(addr, len);
  return 0;
}

// madvise(MADV_DONTNEED): OC and alias overcommit zero-but-keep their backing;
// the ordinary heap-backed arena leaves pages as-is (always RW-backed).
int madvise_fake(void *addr, size_t len, int advice) {
  if (oc_contains(addr) && advice == BIONIC_MADV_DONTNEED) {
    mutexLock(&g_mmap_lock);
    oc_discard_locked(addr, len);
    mutexUnlock(&g_mmap_lock);
    return 0;
  }
  if (g_overcommit && advice == BIONIC_MADV_DONTNEED) arena_dontneed_range(addr, len);
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

char *realpath_fake(const char *path, char *resolved) {
  if (!path) return NULL;          /* POSIX: realpath(NULL,..) is an error, not a crash */
  if (!resolved) resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}
int strerror_r_fake(int err, char *buf, size_t len) { snprintf(buf, len, "%s", strerror(err)); return 0; }
typedef struct {
  uint64_t block_size;
  uint64_t fragment_size;
  uint64_t blocks;
  uint64_t blocks_free;
  uint64_t blocks_available;
  uint64_t files;
  uint64_t files_free;
  uint64_t files_available;
  uint64_t filesystem_id;
  uint64_t flags;
  uint64_t name_max;
  uint32_t reserved[6];
} BionicStatVfs;

typedef struct {
  uint64_t type;
  uint64_t block_size;
  uint64_t blocks;
  uint64_t blocks_free;
  uint64_t blocks_available;
  uint64_t files;
  uint64_t files_free;
  int32_t filesystem_id[2];
  uint64_t name_length;
  uint64_t fragment_size;
  uint64_t flags;
  uint64_t spare[4];
} BionicStatFs;

_Static_assert(sizeof(BionicStatVfs) == 0x70, "bionic statvfs size");
_Static_assert(sizeof(BionicStatFs) == 0x78, "bionic statfs size");

static void filesystem_capacity(const char *path, uint64_t *block_size,
                                uint64_t *fragment_size, uint64_t *blocks,
                                uint64_t *blocks_free,
                                uint64_t *blocks_available,
                                uint64_t *name_max, uint64_t *flags) {
  struct statvfs native;
  const char *query = path && *path ? path : sj_home();
  int result = statvfs(query, &native);
  if (result != 0 && strcmp(query, sj_home()))
    result = statvfs(sj_home(), &native);
  if (result == 0) {
    *block_size = native.f_bsize ? native.f_bsize : 4096;
    *fragment_size = native.f_frsize ? native.f_frsize : *block_size;
    *blocks = native.f_blocks;
    *blocks_free = native.f_bfree;
    *blocks_available = native.f_bavail;
    *name_max = native.f_namemax ? native.f_namemax : 255;
    *flags = native.f_flag;
  } else {
    /* The capacity query itself is optional on some fsdev versions. Do not
     * report a fictitious full disk: Unity refuses to initialize its bundle
     * cache when f_bavail is zero. The actual writes still surface ENOSPC. */
    *block_size = 4096;
    *fragment_size = 4096;
    *blocks = (uint64_t)16 * 1024 * 1024 * 1024 / 4096;
    *blocks_free = (uint64_t)8 * 1024 * 1024 * 1024 / 4096;
    *blocks_available = *blocks_free;
    *name_max = 255;
    *flags = 0;
  }
}

int statvfs_fake(const char *path, void *buffer) {
  if (!buffer) { errno = EFAULT; return -1; }
  BionicStatVfs *out = buffer;
  memset(out, 0, sizeof(*out));
  filesystem_capacity(path, &out->block_size, &out->fragment_size,
                      &out->blocks, &out->blocks_free,
                      &out->blocks_available, &out->name_max, &out->flags);
  out->files = 1u << 20;
  out->files_free = 1u << 19;
  out->files_available = out->files_free;
  return 0;
}

int statfs_fake(const char *path, void *buffer) {
  if (!buffer) { errno = EFAULT; return -1; }
  BionicStatFs *out = buffer;
  memset(out, 0, sizeof(*out));
  uint64_t fragment_size;
  filesystem_capacity(path, &out->block_size, &fragment_size, &out->blocks,
                      &out->blocks_free, &out->blocks_available,
                      &out->name_length, &out->flags);
  out->type = 0x4d44; /* MSDOS_SUPER_MAGIC: libnx SD storage is FAT/exFAT. */
  out->fragment_size = fragment_size;
  out->files = 1u << 20;
  out->files_free = 1u << 19;
  return 0;
}

// Synthetic /proc and /sys files: report a small MemTotal (not the real ~3 GB) via
// /proc/meminfo so Unity's big dynamic-heap reservations stay within our mmap arena,
// and 3 cores via /proc/cpuinfo + the /sys cpu range for the job system.
static const char *synthetic_proc(const char *path) {
  if (!path) return NULL;
  if (!strcmp(path, "/proc/meminfo"))
    return "MemTotal:        524288 kB\n"
           "MemFree:         393216 kB\n"
           "MemAvailable:    393216 kB\n"
           "Buffers:              0 kB\n"
           "Cached:               0 kB\n"
           "SwapTotal:            0 kB\n"
           "SwapFree:             0 kB\n";
  if (!strcmp(path, "/proc/cpuinfo"))
    return "processor\t: 0\nprocessor\t: 1\nprocessor\t: 2\n"
           "Features\t: fp asimd aes pmull sha1 sha2 crc32\n"
           "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\n"
           "CPU part\t: 0xd07\nCPU revision\t: 1\n";
  if (strstr(path, "cpu_capacity")) return "1024\n";
  if (strstr(path, "cpuinfo_max_freq") || strstr(path, "scaling_max_freq")) return "1785000\n";
  if (strstr(path, "cpuinfo_min_freq") || strstr(path, "scaling_min_freq")) return "1020000\n";
  if (strstr(path, "/cpu/possible") || strstr(path, "/cpu/present") || strstr(path, "/cpu/online"))
    return "0-2\n";
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5)) return ""; // empty for the rest
  return NULL;
}

#define PACK_FILE_SLOTS 64
static struct { FILE *file; void *data; int fd; } g_pack_files[PACK_FILE_SLOTS];
static Mutex g_pack_file_lock;

static int cache_path_is(const char *path) {
  return path && strstr(path, "/UnityCache/") != NULL;
}

static int packed_file_add(FILE *file, void *data, int fd) {
  mutexLock(&g_pack_file_lock);
  for (int i = 0; i < PACK_FILE_SLOTS; i++) {
    if (!g_pack_files[i].file) {
      g_pack_files[i].file = file;
      g_pack_files[i].data = data;
      g_pack_files[i].fd = fd;
      mutexUnlock(&g_pack_file_lock);
      return 1;
    }
  }
  mutexUnlock(&g_pack_file_lock);
  return 0;
}

static FILE *packed_fopen(const char *path) {
  void *data = NULL;
  size_t size = 0;
  if (!asset_pack_read_all_path(path, &data, &size)) return NULL;
  FILE *file = fmemopen(data, size ? size : 1, "r");
  int fd = file ? asset_pack_open_path(path) : -1;
  if (!file || !packed_file_add(file, data, fd)) {
    if (fd >= 0) asset_pack_close_fd(fd);
    if (file) fclose(file);
    free(data);
    errno = EMFILE;
    return NULL;
  }
  return file;
}

FILE *fdopen_fake(int fd, const char *mode) {
  if (!asset_pack_fd_is(fd)) return fdopen(fd, mode);
  if (!mode || strpbrk(mode, "wa+")) { errno = EINVAL; return NULL; }
  uint64_t size;
  long position = asset_pack_lseek_fd(fd, 0, SEEK_CUR);
  if (!asset_pack_fstat_fd(fd, &size, NULL, NULL) || size > SIZE_MAX ||
      position < 0)
    return NULL;
  void *data = malloc(size ? (size_t)size : 1);
  if (!data || (size && asset_pack_pread_fd(fd, data, (size_t)size, 0) !=
                        (long)size)) {
    free(data);
    return NULL;
  }
  FILE *file = fmemopen(data, size ? (size_t)size : 1, "r");
  if (!file || fseek(file, position, SEEK_SET) != 0 ||
      !packed_file_add(file, data, fd)) {
    if (file) fclose(file);
    free(data);
    errno = EMFILE;
    return NULL;
  }
  return file;
}

static int packed_fileno(FILE *file) {
  int fd = -1;
  mutexLock(&g_pack_file_lock);
  for (int i = 0; i < PACK_FILE_SLOTS; i++)
    if (g_pack_files[i].file == file) { fd = g_pack_files[i].fd; break; }
  mutexUnlock(&g_pack_file_lock);
  return fd;
}

static int packed_fclose(FILE *file) {
  void *data = NULL;
  int fd = -1;
  mutexLock(&g_pack_file_lock);
  for (int i = 0; i < PACK_FILE_SLOTS; i++) {
    if (g_pack_files[i].file == file) {
      data = g_pack_files[i].data;
      fd = g_pack_files[i].fd;
      g_pack_files[i].file = NULL;
      g_pack_files[i].data = NULL;
      g_pack_files[i].fd = -1;
      break;
    }
  }
  mutexUnlock(&g_pack_file_lock);
  if (!data) return 0;
  int result = fclose(file);
  if (fd >= 0) asset_pack_close_fd(fd);
  free(data);
  return result == 0 ? 1 : -1;
}

// a buffered fopen for the big .mvgl archives: the engine issues many small
// reads/seeks and the fsdev round-trips dominate without a large buffer.
/* Renamed so the tracing wrapper below can count opens and record the last
 * failure. A loading screen that never advances is very often a file the game
 * cannot find, and that is invisible without this. */
static FILE *fopen_fake_inner(const char *path, const char *mode) {
  const char *synth = synthetic_proc(path);
  if (synth) {
    size_t n = strlen(synth);
    void *data = strdup(synth);
    FILE *file = data ? fmemopen(data, n ? n : 1, "r") : NULL;
    if (!file || !packed_file_add(file, data, -1)) {
      if (file) fclose(file);
      free(data);
      return NULL;
    }
    return file;
  }
  const int writing = strpbrk(mode, "wa+") != NULL;
  if (!writing && strchr(mode, 'r')) {
    FILE *packed = packed_fopen(path);
    if (packed) return packed;
  }
  /* Some call sites concatenate the storage path and the filename with no
   * separator ("/switch/sonicjump_nx" + "adspamState.xml"), others insert one.
   * We hand out a base that ends in '/' so the first kind works, which makes
   * the second kind produce a double slash. Collapse them here rather than
   * pick a convention that breaks half the call sites. */
  char norm[512];
  const char *actual_path = path;
  if (strstr(path, "//")) {
    size_t o = 0;
    for (size_t i = 0; path[i] && o < sizeof(norm) - 1; i++) {
      /* A device prefix is "sdmc:/", a colon then ONE slash, so it is never a
       * double slash and needs no special case. An earlier version tried to
       * protect offset 6 and thereby broke the one input it was meant to fix
       * ("sdmc://switch/..."). */
      if (path[i] == '/' && o > 0 && norm[o - 1] == '/')
        continue;
      norm[o++] = path[i];
    }
    norm[o] = '\0';
    actual_path = norm;
  }
  FILE *f = fopen(actual_path, mode);
  if (!f && writing) {            // save file: create the subdir and retry
    mkdir_parents(path);
    f = fopen(actual_path, mode);
  }
  if (!f && !writing && strchr(mode, 'r')) {
    char alt[320];
    /* The game is handed device-less paths ("/switch/...") because
     * managed_path strips the "sdmc:" prefix. libnx mounts sdmc as the default
     * device so those normally resolve, but that depends on the mount having
     * succeeded -- retry explicitly rather than depend on it. */
    if (actual_path[0] == '/') {
      snprintf(alt, sizeof(alt), "sdmc:%s", actual_path);
      f = fopen(alt, mode);
    }
    if (!f && basename_fallback(path, alt, sizeof(alt)))
      f = fopen(alt, mode);
  }
  if (!f) return NULL;
  
  if (strchr(mode, 'r')) {
    const char *ext = strrchr(path, '.');
    if (ext && strcasecmp(ext, ".mvgl") == 0)
      setvbuf(f, NULL, _IOFBF, 256 * 1024);
  }
  return f;
}

FILE *fopen_fake(const char *path, const char *mode) {
  FILE *f;
  sj_mark("fopen");
  f = fopen_fake_inner(path, mode);
  /* Only count reads: the game creates save files on purpose, and counting
   * those as failures would bury the ones that matter. */
  if (mode && *mode == 'r') {
    sj_trace_open(path, f != NULL);
    /* errno distinguishes "this file is genuinely absent" (ENOENT, i.e. the
     * game is probing for an optional override) from a path or device problem,
     * which would be ours to fix. */
    if (!f) {
      static int n;
      if (n < 12) { printf("sj: fopen FAILED errno=%d %s\n", errno, path); n++; }
    }
  }
  return f;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr). libc++_shared wires
// std::cout/cerr/cin to &__sF[1]/[2]/[0]; these wrappers absorb writes to those
// fake FILEs and forward everything else to newlib.
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c (__sF / std{in,out,err})

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
    (void)ptr; (void)size;
    return n;
  }
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}
int fputc_fake(int c, FILE *f) { if (is_fake_file(f)) return c; return fputc(c, f); }
int fputs_fake(const char *s, FILE *f) { if (is_fake_file(f)) {  return 0; } return fputs(s, f); }
int fflush_fake(FILE *f) {
  if (is_fake_file(f) || f == NULL) return 0;
  return fflush(f);
}
int fclose_fake(FILE *f) {
  if (is_fake_file(f)) return 0;
  int packed = packed_fclose(f);
  return packed ? packed < 0 ? -1 : 0 : fclose(f);
}
int ferror_fake(FILE *f) { if (is_fake_file(f)) return 0; return ferror(f); }
int feof_fake(FILE *f) { if (is_fake_file(f)) return 1; return feof(f); }
int fileno_fake(FILE *f) {
  if (is_fake_file(f))
    return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  int packed = packed_fileno(f);
  return packed >= 0 ? packed : fileno(f);
}
int fseek_fake(FILE *f, long off, int whence) {
  if (is_fake_file(f)) return -1;
  return fseek(f, off, whence);
}
int fseeko_fake(FILE *f, long off, int whence) {
  if (is_fake_file(f)) return -1;
  return fseeko(f, (off_t)off, whence);
}
long ftell_fake(FILE *f) { if (is_fake_file(f)) return -1; return ftell(f); }
long ftello_fake(FILE *f) { if (is_fake_file(f)) return -1; return (long)ftello(f); }
int getc_fake(FILE *f) { if (is_fake_file(f)) return -1; return getc(f); }
int fgetc_fake(FILE *f) { if (is_fake_file(f)) return -1; return fgetc(f); }
char *fgets_fake(char *s, int n, FILE *f) { if (is_fake_file(f)) return NULL; return fgets(s, n, f); }
int ungetc_fake(int c, FILE *f) { if (is_fake_file(f)) return -1; return ungetc(c, f); }
void setbuf_fake(FILE *f, char *buf) { if (is_fake_file(f)) return; setbuf(f, buf); }

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va; va_start(va, fmt);
  int ret;
  if (is_fake_file(f)) {
    ret = 0;
  } else {
    ret = vfprintf(f, fmt, va);
  }
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
    (void)fmt; (void)va;
    return 0;
  }
  return vfprintf(f, fmt, va);
}

// ---------------------------------------------------------------------------
// fd routing: the native_app_glue command pipe lives in the fake-fd layer
// (android_native.c). Real files (small fds from open()) pass through to newlib.
// ---------------------------------------------------------------------------

long read_fake(int fd, void *buf, size_t count) {
  if (asset_pack_fd_is(fd)) return asset_pack_read_fd(fd, buf, count);
  if (fakefd_is_fake(fd)) return fakefd_read(fd, buf, count);
  { struct RaCache *c = ra_find(fd); if (c) return ra_read(c, fd, buf, count); }
  /* fsdev can short-read a large read; loop until `count` is satisfied or EOF so
   * il2cpp's global-metadata.dat (which assumes one read() fills the buffer) is
   * never silently truncated. */
  size_t total = 0;
  while (total < count) {
    long r = read(fd, (char *)buf + total, count - total);
    if (r < 0) { if (total) break; return -1; }
    if (r == 0) break; /* EOF */
    total += (size_t)r;
  }
  return (long)total;
}
long pread_fake(int fd, void *buf, size_t count, long off) {
  if (asset_pack_fd_is(fd))
    return asset_pack_pread_fd(fd, buf, count, off);
  size_t total = 0;
  mutexLock(&g_positional_io_lock);
  long cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) {
    mutexUnlock(&g_positional_io_lock);
    return -1;
  }
  while (total < count) {                    /* fsdev can short-read */
    long r = read(fd, (char *)buf + total, count - total);
    if (r < 0) { if (!total) total = (size_t)-1; break; }
    if (r == 0) break;
    total += (size_t)r;
  }
  lseek(fd, cur, SEEK_SET);
  mutexUnlock(&g_positional_io_lock);
  long result = total == (size_t)-1 ? -1 : (long)total;
  return result;
}
long write_fake(int fd, const void *buf, size_t count) {
  if (usym_sink_is(fd)) return (long)count;   /* discard the 176MB .usym copy */
  if (fakefd_is_fake(fd)) return fakefd_write(fd, buf, count);
  size_t total = 0;
  while (total < count) {
    long put = write(fd, (const uint8_t *)buf + total, count - total);
    if (put < 0) {
      return total ? (long)total : -1;
    }
    if (put == 0) break;
    total += (size_t)put;
  }
  return (long)total;
}
int close_fake(int fd) {
  if (asset_pack_fd_is(fd)) {
    fd_ino_clear(fd);
    return asset_pack_close_fd(fd);
  }
  ra_detach(fd);
  fd_ino_clear(fd);
  usym_sink_del(fd);
  if (fakefd_is_fake(fd)) return fakefd_close(fd);
  return close(fd);
}
int pipe_fake(int fds[2]) { return fakefd_pipe(fds); }
// ---------------------------------------------------------------------------
// Networking delegates to the ABI-translated libnx BSD bridge when enabled.
// ---------------------------------------------------------------------------
int poll_fake(void *fds, unsigned long nfds, int timeout) { return nx_poll(fds, nfds, timeout); }
int select_fake(int n, void *r, void *w, void *e, void *t) { return nx_select(n, r, w, e, t); }
int socket_fake(int d, int t, int p) { return nx_socket(d, t, p); }
int connect_fake(int s, const void *a, unsigned l) { return nx_connect(s, a, l); }
int bind_fake(int s, const void *a, unsigned l) { return nx_bind(s, a, l); }
int listen_fake(int s, int b) { return nx_listen(s, b); }
int accept_fake(int s, void *a, void *l) { return nx_accept(s, a, l); }
long send_fake(int s, const void *b, size_t l, int f) { return nx_send(s, b, l, f); }
long recv_fake(int s, void *b, size_t l, int f) { return nx_recv(s, b, l, f); }
long sendto_fake(int s, const void *b, size_t l, int f, const void *a, unsigned al) { return nx_sendto(s, b, l, f, a, al); }
long recvfrom_fake(int s, void *b, size_t l, int f, void *a, void *al) { return nx_recvfrom(s, b, l, f, a, al); }
int shutdown_fake(int s, int how) { return nx_shutdown(s, how); }
int setsockopt_fake(int s, int lv, int n, const void *v, unsigned l) { return nx_setsockopt(s, lv, n, v, l); }
int getsockopt_fake(int s, int lv, int n, void *v, void *l) { return nx_getsockopt(s, lv, n, v, l); }
int getsockname_fake(int s, void *a, void *l) { return nx_getsockname(s, a, l); }
int getpeername_fake(int s, void *a, void *l) { return nx_getpeername(s, a, l); }
int getaddrinfo_fake(const char *node, const char *svc, const void *hints, void **res) { return nx_getaddrinfo(node, svc, hints, res); }
void freeaddrinfo_fake(void *res) { nx_freeaddrinfo(res); }
int getnameinfo_fake(const void *a, unsigned al, char *h, unsigned hl, char *s, unsigned sl, int f) { return nx_getnameinfo(a, al, h, hl, s, sl, f); }
int gethostname_fake(char *name, size_t len) { if (name && len) snprintf(name, len, "switch"); return 0; }
void *getservbyname_fake(const char *n, const char *p) { (void)n; (void)p; return NULL; }
unsigned if_nametoindex_fake(const char *n) { (void)n; return 0; }
char *if_indextoname_fake(unsigned i, char *buf) { (void)i; if (buf) buf[0] = 0; return buf; }
static volatile int g_h_errno = 0;
int *__get_h_errno_fake(void) { return (int *)&g_h_errno; }

// ---------------------------------------------------------------------------
// process control: fork/exec/etc. are unavailable; report failure.
// ---------------------------------------------------------------------------

int fork_fake(void) { errno = ENOSYS; return -1; }
int execvp_fake(const char *f, char *const argv[]) { (void)f; (void)argv; errno = ENOSYS; return -1; }
int waitpid_fake(int pid, int *status, int opts) { (void)pid; (void)opts; if (status) *status = 0; errno = ECHILD; return -1; }
int kill_fake(int pid, int sig) { (void)pid; (void)sig; return 0; }
int getpid_fake(void) { return 1; }
int sched_yield_fake(void) { svcSleepThread(0); return 0; }
// bionic struct passwd layout (pw_dir at +0x20, as the engine derefs).
struct bionic_passwd {
  char *pw_name;     /* 0x00 */
  char *pw_passwd;   /* 0x08 */
  uint32_t pw_uid;   /* 0x10 */
  uint32_t pw_gid;   /* 0x14 */
  char *pw_gecos;    /* 0x18 */
  char *pw_dir;      /* 0x20 */
  char *pw_shell;    /* 0x28 */
};
void *getpwuid_fake(int uid) {
  (void)uid;
  static struct bionic_passwd pw;
  /* dir[] used to be initialised from the compile-time GAME_HOME. The folder
   * is discovered at runtime now, so fill it on first use instead. */
  static char nm[] = "switch", sh[] = "/bin/sh", empty[] = "";
  static char dir[512];
  if (!dir[0]) snprintf(dir, sizeof(dir), "%s", sj_home());
  pw.pw_name = nm; pw.pw_passwd = empty; pw.pw_uid = 0; pw.pw_gid = 0;
  pw.pw_gecos = empty; pw.pw_dir = dir; pw.pw_shell = sh;
  return &pw;
}

// Unity computes its home/cache dir via getenv("HOME") (then getpwuid fallback).
// Serve the writable game root for HOME/TMPDIR; delegate everything else to newlib.
const char *managed_path(const char *p) {
  if (!p) return p;
  const char *c = strchr(p, ':');
  return (c && c[1] == '/') ? c + 1 : p;     // "sdmc:/switch/.." -> "/switch/.."
}
char *getenv_fake(const char *name) {
  if (name) {
    if (!strcmp(name, "HOME"))   return (char *)managed_path(sj_home());
    if (!strcmp(name, "TMPDIR")) return (char *)managed_path(sj_home());
  }
  return getenv(name);
}
// Report a Unix-rooted cwd (no "sdmc:") so managed Path APIs don't treat it as
// relative in Path.Combine. newlib's internal cwd is unchanged, so relative reads
// still resolve via the default device.
char *getcwd_fake(char *buf, size_t size) {
  char *r = getcwd(buf, size);
  if (!r) return r;
  const char *c = strchr(r, ':');
  if (c && c[1] == '/') memmove(r, c + 1, strlen(c + 1) + 1);  // drop "sdmc:"
  return r;
}
int getrusage_fake(int who, void *usage) { (void)who; if (usage) memset(usage, 0, 144); return 0; }

// ---------------------------------------------------------------------------
// dlopen/dlsym over the already-loaded modules (no real dynamic loading).
// dlsym lets the engine look up its own exports / our shims.
// ---------------------------------------------------------------------------

void *dlopen_fake(const char *name, int flags) {
  (void)name;
  (void)flags;
  return (void *)0x1;
}
int dlclose_fake(void *h) { (void)h; return 0; }
const char *dlerror_fake(void) { return NULL; }
void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol) return NULL;
  /* Firebase SWIG stub resolver (firebase_stub.c) -- see step 2b below. */
  extern void *firebase_stub_lookup(const char *symbol);
  /* 1) a real export from a loaded module (il2cpp/unity/main) */
  void *p = so_resolve_external(symbol);
  if (p) return p;
  /* 2) one of our libc/GLES/EGL shims (the engine dlopen()s libGLESv2.so etc.
   *    and dlsym()s glGetString/glGetIntegerv, which are shims, not exports) */
  uintptr_t shim = dynlib_find_export(symbol);
  if (shim) {  return (void *)shim; }
  /* 2b) Firebase SWIG P/Invokes: the real Firebase .so files are intentionally not
   *     loaded, so answer with trivial stubs that resolve the dependency check to
   *     Available(0) and let the bootstrap advance. */
  void *fb = firebase_stub_lookup(symbol);
  if (fb) return fb;
  /* 3) the full GLES/EGL API (~150 entry points) lives in mesa, beyond our
   *    static table -- resolve any gl or egl symbol via eglGetProcAddress. */
  if (!strncmp(symbol, "gl", 2) || !strncmp(symbol, "egl", 3)) {
    p = (void *)eglGetProcAddress(symbol);
    if (p) {  return p; }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks, semaphores, timed locks
// ---------------------------------------------------------------------------

typedef struct { RwLock lock; } FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) { FakeRwLock *l = calloc(1, sizeof(*l)); rwlockInit(&l->lock); *storage = l; }
  return *storage;
}
int pthread_rwlock_rdlock_fake(void **rw) { RwLock *l=&get_rwlock(rw)->lock; rwlockReadLock(l); return 0; }
int pthread_rwlock_wrlock_fake(void **rw) { RwLock *l=&get_rwlock(rw)->lock; rwlockWriteLock(l); return 0; }
int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock)) rwlockWriteUnlock(&l->lock);
  else rwlockReadUnlock(&l->lock);
  return 0;
}

typedef struct { Semaphore sem; } FakeSem;
int sem_init_fake(void **s, int pshared, unsigned int value) { (void)pshared; FakeSem *fs = calloc(1, sizeof(*fs)); semaphoreInit(&fs->sem, value); *s = fs; return 0; }
int sem_destroy_fake(void **s) { if (s && *s) { free(*s); *s = NULL; } return 0; }
int sem_post_fake(void **s) { if (s && *s) semaphoreSignal(&((FakeSem *)*s)->sem); return 0; }
int sem_wait_fake(void **s) { if (s && *s) semaphoreWait(&((FakeSem *)*s)->sem); return 0; }
int sem_trywait_fake(void **s) {
  /* Non-blocking, but the engine polls it in a loop while waiting for a JNI
   * result, so a breadcrumb here distinguishes "spinning on a semaphore that
   * will never be posted" from "genuinely blocked". */
  sj_mark("sem_trywait");
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem)) return 0;
  errno = EAGAIN; return -1;
}
int sem_getvalue_fake(void **s, int *val) { if (s && *s) *val = (int)((FakeSem *)*s)->sem.count; else *val = 0; return 0; }
// no native timed wait on libnx Semaphore; poll with a short backoff to the
// deadline. The engine uses it as a yield-with-timeout in its task scheduler.
int sem_timedwait_fake(void **s, const struct timespec *abs) {
  (void)abs;
  for (int i = 0; i < 1000; i++) {
    if (sem_trywait_fake(s) == 0) return 0;
    svcSleepThread(1000000ull); // 1 ms
  }
  errno = ETIMEDOUT;
  return -1;
}

/* --- Boehm GC stop-the-world bridge -------------------------------------
 * il2cpp's Boehm GC stops the world by pthread_kill'ing every other thread; each
 * handler sem_posts an ack that GC_stop_world/GC_start_world wait on. POSIX signals
 * are never delivered on Switch, so the acks never arrive and the first collection
 * hangs forever -- the verified boot wall. Fix: make pthread_kill itself post the
 * ack the never-delivered handler would have (every suspended thread is already
 * parked in our shim). The signals, gate and ack-sem are il2cpp globals that read
 * 0/NULL before GC init, so the bridge stays inert until the GC is up. */
uintptr_t g_il2cpp_base = 0;

/* Journey 3.8.4 / Unity 2022.3.67f2 Boehm GC signal globals.  pthread_kill()
 * cannot deliver Android's suspend/restart handlers on Horizon, so acknowledge
 * those two signals through the collector's own semaphore. */
#define GC_START_ACK_OFF   0x049b1bc8
#define GC_SUSPEND_SIG_OFF 0x049b1bcc
#define GC_RESTART_SIG_OFF 0x049b1bd0
#define GC_ACK_SEM_OFF     0x04bd9700

int pthread_kill_gc(pthread_t t, int sig) {
  (void)t;
  uintptr_t b = g_il2cpp_base;
  if (b && sig) {
    int suspend_sig = *(volatile int *)(b + GC_SUSPEND_SIG_OFF);
    int restart_sig = *(volatile int *)(b + GC_RESTART_SIG_OFF);
    void **ack_sem  = (void **)(b + GC_ACK_SEM_OFF);
    if (sig == suspend_sig) {            /* stop-the-world: ack the suspend */
      sem_post_fake(ack_sem);
      return 0;
    }
    if (sig == restart_sig) {            /* start-the-world: ack iff handler would */
      if (*(volatile int *)(b + GC_START_ACK_OFF)) sem_post_fake(ack_sem);
      return 0;
    }
  }
  return 0;   /* any other signal: no-op, as before */
}
