/* pps_shim.c -- see pps_shim.h. MIT licensed. */

#include <switch.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include "config.h"
#include <time.h>
#include <errno.h>
#include <locale.h>

#include <wchar.h>
#include <wctype.h>
#include <stdio.h>
#include <stdint.h>
#include <setjmp.h>
#include <sys/time.h>
#include <signal.h>
#include "pps_shim.h"
#include "fakefd.h"

/* sincos/sincosf moved to imports_helpers.c, which owns every entry the table
 * names *_fake, so there is one place to look for them. */

/* Android/bionic and devkitA64/newlib assign different numeric clock IDs:
 *
 *     bionic CLOCK_REALTIME  = 0     newlib CLOCK_REALTIME  = 1
 *     bionic CLOCK_MONOTONIC = 1     newlib CLOCK_MONOTONIC = 4
 *
 * The overlap is the danger: an untranslated id still returns A time, so
 * nothing fails loudly -- the engine's frame clock just follows the wall clock
 * and jumps whenever the system time changes. Translate properly rather than
 * relying on the numbers happening to line up.
 *
 * (The Osmos port established this against its own binary, which passed 1 from
 * both GetTickCount and mach_continuous_time. Which ids THIS engine passes has
 * not been traced -- the translation is correct for all of them, so it did not
 * need to be.) */
int clock_gettime_bionic(int android_id, struct timespec *tp) {
  if (!tp) { errno = EINVAL; return -1; }

  clockid_t host;
  switch (android_id) {
    case 0:  /* CLOCK_REALTIME          */
    case 5:  /* CLOCK_REALTIME_COARSE   */
      host = CLOCK_REALTIME; break;
    case 1:  /* CLOCK_MONOTONIC         */
    case 2:  /* CLOCK_PROCESS_CPUTIME_ID: elapsed monotonic is close enough */
    case 3:  /* CLOCK_THREAD_CPUTIME_ID */
    case 4:  /* CLOCK_MONOTONIC_RAW     */
    case 6:  /* CLOCK_MONOTONIC_COARSE  */
    case 7:  /* CLOCK_BOOTTIME          */
      host = CLOCK_MONOTONIC; break;
    default:
      errno = EINVAL; return -1;
  }
  return clock_gettime(host, tp);
}

/* clock_getres MUST report 1 ns. This is not a hardware question.
 *
 * The engine derives its timebase from it:
 *
 *     CCounter::GetPerformanceCounter()  -> mach_continuous_time() -> NANOSECONDS
 *     CCounter::GetPerformanceFrequency() -> 1e9 / clock_getres().tv_nsec
 *     CCounter::GetSecs()                 -> counter / frequency
 *
 * The counter is already in nanoseconds, so the only frequency that makes
 * GetSecs() correct is 1e9 -- which requires tv_nsec == 1. That is exactly
 * what bionic returns for CLOCK_MONOTONIC on Android, the platform this engine
 * was built and tuned against.
 *
 * An earlier version returned 6, reasoning that it should report "the real
 * hardware tick rather than claiming 1 ns and having the engine compute a
 * nonsense frame budget". That was backwards: the engine is not asking how
 * precise the clock is, it is asking for the divisor that converts its
 * nanosecond counter to seconds. Returning 6 made the frequency 166,666,666
 * instead of 1,000,000,000, so every CCounter-derived duration ran SIX TIMES
 * fast. (The 6 was not even right on its own terms -- the Switch system
 * counter is 19.2 MHz, or 52 ns, not the 192 MHz the comment claimed.)
 */
int clock_getres_fake(int clk, struct timespec *res) {
  (void)clk;
  if (!res) { errno = EFAULT; return -1; }
  res->tv_sec  = 0;
  res->tv_nsec = 1;
  return 0;
}

/* setlocale category numbering is not merely different between bionic and
 * newlib, it is a different ordering entirely:
 *
 *     bionic:  LC_CTYPE 0  LC_NUMERIC 1  LC_TIME 2  LC_COLLATE 3
 *              LC_MONETARY 4  LC_MESSAGES 5  LC_ALL 6
 *     newlib:  LC_ALL 0  LC_COLLATE 1  LC_CTYPE 2  LC_MONETARY 3
 *              LC_NUMERIC 4  LC_TIME 5  LC_MESSAGES 6
 *
 * libosmos.so calls setlocale(6, ...) from std::__ndk1::locale::global --
 * that is bionic's LC_ALL, which newlib reads as LC_MESSAGES. Passing it
 * through unchanged sets the wrong category and, when the name is one newlib
 * does not know, returns NULL. libc++ treats a NULL return from locale::global
 * as grounds to throw. Translate instead. */
static int lc_bionic_to_newlib(int c) {
  switch (c) {
    case 0: return LC_CTYPE;
    case 1: return LC_NUMERIC;
    case 2: return LC_TIME;
    case 3: return LC_COLLATE;
    case 4: return LC_MONETARY;
    case 5: return LC_MESSAGES;
    case 6: return LC_ALL;
    default: return LC_ALL;
  }
}

char *setlocale_bionic(int category, const char *locale) {
  return setlocale(lc_bionic_to_newlib(category), locale);
}

/* Osmos parses its .cfg and .loc files through the C locale only -- it never
 * calls newlocale with anything but LC_GLOBAL_LOCALE -- so ignoring the locale
 * argument here is correct rather than merely convenient. */
int isdigit_l_fake(int c, void *loc)  { (void)loc; return isdigit(c);  }
int islower_l_fake(int c, void *loc)  { (void)loc; return islower(c);  }
int isupper_l_fake(int c, void *loc)  { (void)loc; return isupper(c);  }
int isxdigit_l_fake(int c, void *loc) { (void)loc; return isxdigit(c); }
int tolower_l_fake(int c, void *loc)  { (void)loc; return tolower(c);  }
int toupper_l_fake(int c, void *loc)  { (void)loc; return toupper(c);  }

/* ------------------------------------------------------------------ */
/* mmap / munmap                                                       */
/* ------------------------------------------------------------------ */

/* WHAT WAS AND WAS NOT CHECKED FOR THIS LIBRARY
 *
 * Checked against libpapapearsaga.so's import table: it imports mmap, munmap
 * and mremap, and does NOT import mprotect, madvise or msync. FreeType is
 * present (ft_heap_limit and ft_quick_sort are in .rodata), so the same
 * font-mapping path the Osmos port traced almost certainly exists here.
 *
 * NOT checked: which call sites reach mmap. The Osmos port cross-referenced
 * every PLT call site in ITS binary and found exactly one -- FreeType's
 * FT_Stream_Open mapping a font read-only -- but that was a 27x smaller
 * library and this one imports mremap, which that one did not. So the
 * simplifying assumption below is inherited rather than verified, and if the
 * engine turns out to map something larger or to grow a mapping, this is the
 * first place to look.
 *
 * libc_shim's arena is roughly 700 lines written for Unity. Its own comment
 * says why: Unity reserves big aligned pools by over-mapping and then
 * munmapping the head and tail, so a plain malloc/free per mmap would free the
 * whole block when the head is trimmed. FreeType does none of that. It maps a
 * file, reads it, and unmaps exactly what it mapped.
 *
 * Serving that with the Unity machinery produced three separate failures here:
 * an unbreakable infinite loop when the granule was left at zero; the
 * over-map/trim path being taken for a 3.2 MB font because the threshold sat
 * below it; and a large arena carved out of the same newlib heap the engine
 * allocates from, after which malloc(400) returned an unmapped address and
 * FT_Init_FreeType failed.
 *
 * The requirement is: allocate, fill from the file, remember the size, free on
 * unmap. That is what this does. The arena code stays in the tree, unmodified
 * and unused, so upstream fixes can still be pulled into it.
 */

#define OSMOS_MAP_ANONYMOUS 0x20      /* bionic MAP_ANONYMOUS */
#define OSMOS_MAP_FAILED    ((void *)-1)
#define PPS_MAX_MAPS      32

static struct { void *p; size_t len; } g_maps[PPS_MAX_MAPS];
static Mutex g_maps_lock;

void *pps_mmap(void *addr, size_t len, int prot, int flags,
                 int fd, long offset) {
  (void)addr; (void)prot;
  if (len == 0) { errno = EINVAL; return OSMOS_MAP_FAILED; }

  /* Page-aligned because callers assume it, and because FreeType computes the
   * face pointer as an offset from the mapping base. */
  void *p = memalign(0x1000, len);
  if (!p) { errno = ENOMEM; return OSMOS_MAP_FAILED; }

  if (flags & OSMOS_MAP_ANONYMOUS) {
    memset(p, 0, len);                    /* anonymous memory reads as zero */
  } else if (fd >= 0) {
    /* Fill from the file. A short read is not an error -- a mapping may run
     * past EOF -- but the tail must be zero rather than whatever memalign
     * handed back, or FreeType parses uninitialised memory as font data. */
    const long saved = lseek(fd, 0, SEEK_CUR);
    size_t got = 0;
    if (lseek(fd, offset, SEEK_SET) >= 0) {
      while (got < len) {
        const long r = read(fd, (char *)p + got, len - got);
        if (r <= 0) break;
        got += (size_t)r;
      }
    }
    if (got < len) memset((char *)p + got, 0, len - got);
    if (saved >= 0) lseek(fd, saved, SEEK_SET);
  } else {
    memset(p, 0, len);
  }

  mutexLock(&g_maps_lock);
  int slot = -1;
  for (int i = 0; i < PPS_MAX_MAPS; i++)
    if (!g_maps[i].p) { slot = i; break; }
  if (slot >= 0) { g_maps[slot].p = p; g_maps[slot].len = len; }
  mutexUnlock(&g_maps_lock);

  if (slot < 0) {
    /* Out of table slots. Leaking is survivable; handing back a pointer that
     * munmap cannot free and free() would reject is not. */
    LOGW("mmap: tracking table full (%d live); leaking %u KB",
         PPS_MAX_MAPS, (unsigned)(len >> 10));
  }
  return p;
}

int pps_munmap(void *addr, size_t len) {
  (void)len;
  if (!addr || addr == OSMOS_MAP_FAILED) return 0;

  mutexLock(&g_maps_lock);
  void *found = NULL;
  for (int i = 0; i < PPS_MAX_MAPS; i++) {
    if (g_maps[i].p == addr) {
      found = g_maps[i].p;
      g_maps[i].p = NULL;
      g_maps[i].len = 0;
      break;
    }
  }
  mutexUnlock(&g_maps_lock);

  /* Only free pointers this shim handed out. Calling free() on anything else
   * is precisely the kind of heap corruption that cost this port a week. */
  if (found) free(found);
  return 0;
}

/* ==================================================================== */
/* ctype thunks                                                         */
/* ==================================================================== */

/* newlib defines several of these as macros, so the module cannot take their
 * address. Every one is a thunk over the standard behaviour. */
int pps_isalnum(int c)  { return isalnum(c);  }
int pps_isalpha(int c)  { return isalpha(c);  }
int pps_isgraph(int c)  { return isgraph(c);  }
int pps_islower(int c)  { return islower(c);  }
int pps_isprint(int c)  { return isprint(c);  }
int pps_isspace(int c)  { return isspace(c);  }
int pps_isupper(int c)  { return isupper(c);  }
int pps_isxdigit(int c) { return isxdigit(c); }
int pps_tolower(int c)  { return tolower(c);  }
int pps_toupper(int c)  { return toupper(c);  }

int pps_iswalpha(unsigned c)  { return iswalpha((wint_t)c);  }
int pps_iswblank(unsigned c)  { return iswblank((wint_t)c);  }
int pps_iswcntrl(unsigned c)  { return iswcntrl((wint_t)c);  }
int pps_iswdigit(unsigned c)  { return iswdigit((wint_t)c);  }
int pps_iswlower(unsigned c)  { return iswlower((wint_t)c);  }
int pps_iswprint(unsigned c)  { return iswprint((wint_t)c);  }
int pps_iswpunct(unsigned c)  { return iswpunct((wint_t)c);  }
int pps_iswspace(unsigned c)  { return iswspace((wint_t)c);  }
int pps_iswupper(unsigned c)  { return iswupper((wint_t)c);  }
int pps_iswxdigit(unsigned c) { return iswxdigit((wint_t)c); }
unsigned pps_towlower(unsigned c) { return (unsigned)towlower((wint_t)c); }
unsigned pps_towupper(unsigned c) { return (unsigned)towupper((wint_t)c); }

long double pps_strtold_l(const char *s, char **end, void *loc) {
  (void)loc; return strtold(s, end);
}
long long pps_strtoll_l(const char *s, char **end, int base, void *loc) {
  (void)loc; return strtoll(s, end, base);
}
unsigned long long pps_strtoull_l(const char *s, char **end, int base, void *loc) {
  (void)loc; return strtoull(s, end, base);
}

/* ==================================================================== */
/* Signals                                                              */
/* ==================================================================== */

/* All inert, all reporting success.
 *
 * King's engine installs a SIGSEGV/SIGBUS handler on an alternate stack and
 * writes a crash report that NativeApplication.getCrashReportIfAvailable
 * reads back on the following launch. On this console none of that can work:
 * there is no POSIX signal delivery for a CPU fault, so the handler would
 * never run, and if it somehow did it would have no unwinder to walk with.
 *
 * Success rather than failure is the deliberate choice. Some builds of this
 * engine treat a failed sigaction as fatal and refuse to finish startup, so
 * returning -1 here would trade a working port for an honest error nobody
 * can act on. A libnx crash dump is more useful than the report would have
 * been anyway. */
int pps_sigaction(int sig, const void *act, void *old) {
  (void)sig; (void)act;
  /* The engine chains handlers: it reads the old one back and calls it from
   * its own. Zeroing means "there was no previous handler", which stops it
   * dereferencing whatever happened to be on the stack. */
  if (old) memset(old, 0, 152);   /* bionic sizeof(struct sigaction) */
  return 0;
}

int pps_sigaltstack(const void *ss, void *old) {
  (void)ss;
  if (old) memset(old, 0, 24);    /* bionic sizeof(stack_t) */
  return 0;
}

int pps_sigemptyset(void *set) { if (set) memset(set, 0, 8); return 0; }
int pps_sigfillset(void *set)  { if (set) memset(set, 0xff, 8); return 0; }

/* SIG_ERR is (void*)-1 and SIG_DFL is (void*)0. Report that the previous
 * handler was the default, not an error: the engine tests for SIG_ERR. */
void *pps_signal(int sig, void *handler) { (void)sig; (void)handler; return NULL; }

int pps_raise(int sig) { (void)sig; return 0; }

/* ==================================================================== */
/* Process, terminal, filesystem metadata                               */
/* ==================================================================== */

/* The engine calls ptrace(PTRACE_TRACEME) and treats failure as "a debugger is
 * already attached". Returning 0 (success) is the "no debugger" answer. */
long pps_ptrace(int request, ...) { (void)request; return 0; }

/* Only reachable from the crash reporter's relaunch path, which has nowhere
 * to relaunch to. -1 is "could not exec", which is true. */
int pps_execl(const char *path, const char *arg, ...) {
  (void)path; (void)arg; errno = ENOSYS; return -1;
}

int pps_tcgetattr(int fd, void *t) { (void)fd; if (t) memset(t, 0, 60); return 0; }
int pps_tcsetattr(int fd, int act, const void *t) { (void)fd; (void)act; (void)t; return 0; }
int pps_ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; errno = ENOTTY; return -1; }
unsigned pps_alarm(unsigned sec) { (void)sec; return 0; }

int pps_socketpair(int d, int t, int p, int sv[2]) {
  /* Not a socket: the engine only ever uses a socketpair as a self-pipe to
   * wake a blocked worker, and fakefd's pipe does exactly that. Handing it a
   * real pair would need a network stack we deliberately do not bring up. */
  (void)d; (void)t; (void)p;
  return fakefd_pipe(sv);
}

int pps_getpagesize(void) { return 0x1000; }

long long pps_lseek64(int fd, long long off, int whence) {
  return (long long)lseek(fd, (off_t)off, whence);
}

/* The SD card has no ownership or permission model, so these succeed without
 * doing anything. Failing would make the engine treat its own save directory
 * as unwritable. */
int pps_chmod(const char *p, unsigned m)  { (void)p; (void)m; return 0; }
int pps_fchmod(int fd, unsigned m)        { (void)fd; (void)m; return 0; }
int pps_fchown(int fd, unsigned u, unsigned g) { (void)fd; (void)u; (void)g; return 0; }
unsigned pps_umask(unsigned m)            { (void)m; return 0; }
int pps_utimes(const char *p, const void *tv) { (void)p; (void)tv; return 0; }

long pps_readlink(const char *p, char *buf, size_t n) {
  (void)p; (void)buf; (void)n; errno = EINVAL; return -1;   /* not a symlink */
}

/* A fixed non-root uid. The engine only ever formats these into log lines and
 * into the path it would have used for external storage. */
unsigned pps_getuid(void)  { return 1000; }
unsigned pps_geteuid(void) { return 1000; }
unsigned pps_getgid(void)  { return 1000; }
unsigned pps_getegid(void) { return 1000; }

int pps_dup2(int a, int b) { (void)a; (void)b; errno = ENOSYS; return -1; }
int pps_fsync(int fd)      { (void)fd; return 0; }
int pps_ftruncate(int fd, long len) { (void)fd; (void)len; return 0; }

/* <syslog.h> is a glibc header devkitA64 does not ship, so these are declared
 * in pps_shim.h rather than included from anywhere. */
void pps_openlog(const char *ident, int opt, int fac) { (void)ident; (void)opt; (void)fac; }
void pps_syslog(int pri, const char *fmt, ...)        { (void)pri; (void)fmt; }
void pps_closelog(void) { }

/* dladdr is used by the engine's own backtrace formatter. Reporting failure
 * makes it print a bare address, which is what tools/symbolize.py wants. */
int pps_dladdr(const void *addr, void *info) { (void)addr; (void)info; return 0; }

/* ==================================================================== */
/* rwlocks                                                              */
/* ==================================================================== */

/* newlib has no rwlocks. A plain mutex on both paths is pessimistic but never
 * wrong, and the engine only uses these around its resource tables.
 *
 * bionic's pthread_rwlock_t is 56 bytes of inline storage that the engine
 * zero-initialises, exactly like its mutexes -- so the same trick imports_
 * helpers.c uses applies: reinterpret the caller's storage as a lazily
 * allocated pointer slot rather than trying to fit a libnx RMutex into a
 * layout that was not designed for one. */
typedef struct { uint64_t magic; RMutex m; } PpsRwlock;
#define PPS_RW_MAGIC 0x5057524C4F434B31ULL   /* 'PWRLOCK1' */

static PpsRwlock *rw_get(void *rw) {
  PpsRwlock **slot = (PpsRwlock **)rw;
  if (!slot) return NULL;
  if (!*slot || (*slot)->magic != PPS_RW_MAGIC) {
    PpsRwlock *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->magic = PPS_RW_MAGIC;
    rmutexInit(&l->m);
    *slot = l;
  }
  return *slot;
}

int pps_rwlock_init(void *rw, const void *attr) {
  (void)attr;
  if (rw) *(void **)rw = NULL;   /* force a fresh lazy allocation */
  return rw_get(rw) ? 0 : -1;
}
int pps_rwlock_rdlock(void *rw) { PpsRwlock *l = rw_get(rw); if (!l) return -1; rmutexLock(&l->m); return 0; }
int pps_rwlock_wrlock(void *rw) { return pps_rwlock_rdlock(rw); }
int pps_rwlock_unlock(void *rw) { PpsRwlock *l = rw_get(rw); if (!l) return -1; rmutexUnlock(&l->m); return 0; }
int pps_rwlock_destroy(void *rw) {
  PpsRwlock **slot = (PpsRwlock **)rw;
  if (slot && *slot && (*slot)->magic == PPS_RW_MAGIC) { free(*slot); *slot = NULL; }
  return 0;
}

/* ==================================================================== */
/* __sF                                                                 */
/* ==================================================================== */

/* ==================================================================== */
/* mremap                                                               */
/* ==================================================================== */

/* No newlib equivalent. The engine's allocator grows a mapping rather than
 * remapping it in place, so copy-and-free is a faithful implementation --
 * mremap without MREMAP_MAYMOVE is allowed to fail, and with it the address is
 * explicitly permitted to change. */
void *pps_mremap(void *old, size_t oldsz, size_t newsz, int flags, ...) {
  void *fresh;
  (void)flags;
  if (!old) return pps_mmap(NULL, newsz, 3, 0x22, -1, 0);
  if (newsz <= oldsz) return old;
  fresh = pps_mmap(NULL, newsz, 3, 0x22, -1, 0);
  if (!fresh || fresh == (void *)-1) return (void *)-1;
  memcpy(fresh, old, oldsz);
  pps_munmap(old, oldsz);
  return fresh;
}

/* ------------------------------------------------------------------ */
/* ctype thunks                                                        */
/* ------------------------------------------------------------------ */

/* One line each, and they exist for a mechanical reason rather than a
 * behavioural one: newlib defines these as macros over __ctype_ptr__, so
 * `&isspace` is not something a caller can take. bionic exports real
 * functions, so the module's import table asks for an address that newlib
 * does not have to give. Wrapping each in a function creates one.
 *
 * The macro still applies inside the body, so these compile down to the same
 * table lookup the macro would have produced. */
int isalnum_fn(int c)  { return isalnum(c);  }
int isalpha_fn(int c)  { return isalpha(c);  }
int isgraph_fn(int c)  { return isgraph(c);  }
int islower_fn(int c)  { return islower(c);  }
int isprint_fn(int c)  { return isprint(c);  }
int isspace_fn(int c)  { return isspace(c);  }
int isupper_fn(int c)  { return isupper(c);  }
int isxdigit_fn(int c) { return isxdigit(c); }
int tolower_fn(int c)  { return tolower(c);  }
int toupper_fn(int c)  { return toupper(c);  }

int iswalpha_fn(wint_t c)  { return iswalpha(c);  }
int iswblank_fn(wint_t c)  { return iswblank(c);  }
int iswcntrl_fn(wint_t c)  { return iswcntrl(c);  }
int iswdigit_fn(wint_t c)  { return iswdigit(c);  }
int iswlower_fn(wint_t c)  { return iswlower(c);  }
int iswprint_fn(wint_t c)  { return iswprint(c);  }
int iswpunct_fn(wint_t c)  { return iswpunct(c);  }
int iswspace_fn(wint_t c)  { return iswspace(c);  }
int iswupper_fn(wint_t c)  { return iswupper(c);  }
int iswxdigit_fn(wint_t c) { return iswxdigit(c); }
wint_t towlower_fn(wint_t c) { return towlower(c); }
wint_t towupper_fn(wint_t c) { return towupper(c); }

/* ------------------------------------------------------------------ */
/* deliberately inert                                                  */
/* ------------------------------------------------------------------ */

/* Signals.
 *
 * The engine installs a SIGSEGV/SIGABRT handler and an alternate stack so it
 * can write its own crash report -- that is what NativeApplication's
 * getCrashReportIfAvailable/removeCrashReport pair is for. On Android that
 * report was uploaded; here there is nowhere to send it, and a handler
 * belonging to a module we relocated by hand is the last thing that should be
 * standing between a fault and libnx's own diagnostic.
 *
 * Reporting SUCCESS rather than failure is deliberate. The engine checks the
 * return of sigaction and logs loudly on failure, and on some paths retries;
 * claiming the handler was installed and then never calling it is quieter and
 * has the same effect. Nothing downstream depends on the handler running,
 * because on a healthy run it never would. */
int sigaction_stub(int sig, const void *act, void *old) {
  (void)sig; (void)act;
  /* The engine reads back the old handler on some paths and stores it to
   * chain to. Zeroing it means "there was no previous handler", which is both
   * true here and the case its own code already handles. */
  if (old) memset(old, 0, sizeof(struct sigaction));
  return 0;
}
int   sigaltstack_stub(const void *ss, void *old) { (void)ss; if (old) memset(old, 0, sizeof(stack_t)); return 0; }
int   sigemptyset_stub(void *set) { if (set) memset(set, 0, sizeof(sigset_t)); return 0; }
int   sigfillset_stub(void *set)  { if (set) memset(set, 0xff, sizeof(sigset_t)); return 0; }

/* raise() is NOT routed to newlib's. The engine calls it to abort after its
 * own handler has written a report, and newlib's would take the process down
 * through a path libnx cannot describe. Log and continue instead: whatever
 * made it call raise is already in debug.log by this point. */
int raise_stub(int sig) {
  LOGE("the engine called raise(%d); ignored -- see the lines above for why", sig);
  return 0;
}

/* Process, ownership and terminal. Present because bionic had them. */
long ptrace_stub(int req, ...) {
  /* Android anti-debug probes call PTRACE_TRACEME and treat failure as "a
   * debugger is already attached". Report success so that check passes. */
  (void)req; return 0;
}
int      execl_stub(const char *path, const char *arg, ...) { (void)path; (void)arg; errno = ENOSYS; return -1; }
unsigned getuid_stub(void) { return 1000; }   /* a plausible Android app uid */
unsigned getgid_stub(void) { return 1000; }
int      tcgetattr_stub(int fd, void *t) { (void)fd; (void)t; errno = ENOTTY; return -1; }
int      tcsetattr_stub(int fd, int act, const void *t) { (void)fd; (void)act; (void)t; errno = ENOTTY; return -1; }
int      ioctl_stub(int fd, unsigned long req, ...) { (void)fd; (void)req; errno = ENOTTY; return -1; }
unsigned alarm_stub(unsigned sec) { (void)sec; return 0; }
int      umask_stub(int mask) { (void)mask; return 0; }
int      chmod_stub(const char *p, int mode) { (void)p; (void)mode; return 0; }
int      fchmod_stub(int fd, int mode) { (void)fd; (void)mode; return 0; }
int      fchown_stub(int fd, unsigned uid, unsigned gid) { (void)fd; (void)uid; (void)gid; return 0; }
int      socketpair_stub(int d, int t, int p, int sv[2]) { (void)d; (void)t; (void)p; (void)sv; errno = ENOSYS; return -1; }
void     openlog_stub(const char *ident, int opt, int fac) { (void)ident; (void)opt; (void)fac; }
void     syslog_stub(int pri, const char *fmt, ...) { (void)pri; (void)fmt; }
void     closelog_stub(void) { }

/* ------------------------------------------------------------------ */
/* small gaps in newlib                                                */
/* ------------------------------------------------------------------ */

int  getpagesize_fn(void) { return 0x1000; }

/* newlib's off_t is already 64-bit on aarch64, so this is lseek under another
 * name rather than a separate large-file entry point. */
long lseek64_fn(int fd, long off, int whence) { return lseek(fd, off, whence); }

void *memrchr_fn(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  while (n--) if (p[n] == (unsigned char)c) return (void *)(p + n);
  return NULL;
}

/* POSIX basename, which may modify its argument, rather than the GNU variant.
 * bionic ships the POSIX one, and the engine passes a writable buffer. */
char *basename_fn(char *path) {
  char *slash;
  if (!path || !*path) return (char *)".";
  slash = strrchr(path, '/');
  if (!slash) return path;
  if (slash[1]) return slash + 1;
  /* trailing slash: trim and retry */
  while (slash > path && *slash == '/') *slash-- = 0;
  slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/* ------------------------------------------------------------------ */
/* the reference ports' own bodies, reimplemented here                 */
/* ------------------------------------------------------------------ */

int pps_gettimeofday(void *tv, void *tz) {
  /* newlib's gettimeofday exists but goes through the RTC service, which is
   * both slow and coarse. The engine calls this on every frame for its own
   * timing, so it is served from the CPU tick counter instead -- the same
   * source main.c uses for dt, which keeps the two consistent. */
  struct timeval *out = (struct timeval *)tv;
  (void)tz;
  if (out) {
    const u64 ns = armTicksToNs(armGetSystemTick());
    out->tv_sec  = (time_t)(ns / 1000000000ULL);
    out->tv_usec = (suseconds_t)((ns % 1000000000ULL) / 1000ULL);
  }
  return 0;
}

unsigned pps_sleep(unsigned seconds) {
  svcSleepThread((u64)seconds * 1000000000ULL);
  return 0;
}

/* newlib has setjmp/longjmp but not the sig* pair -- there is no signal mask
 * to save here, so they are the plain versions under another name. The engine
 * uses them in its own exception path, which on a healthy run never unwinds. */
int pps_sigsetjmp(void *env, int savemask) {
  (void)savemask;
  return setjmp(*(jmp_buf *)env);
}
void pps_siglongjmp(void *env, int val) {
  longjmp(*(jmp_buf *)env, val ? val : 1);
}

/* bionic allocates a pthread_rwlock_t inline and zero-initialises it, so there
 * is nothing to construct. libc_shim provides rdlock/wrlock/unlock over that
 * storage; this only has to agree that the lock now exists. */
int pthread_rwlock_init_stub(void **rw, const void *attr) {
  (void)attr;
  if (rw) *rw = NULL;   /* libc_shim's lazily-backed pointer slot */
  return 0;
}
