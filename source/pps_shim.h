/* pps_shim.h -- the handful of libc entry points neither reference port
 * covers and newlib does not provide. MIT licensed.
 *
 * Everything here exists because bionic has it and devkitA64's newlib does
 * not, or has it only as a macro. Nothing here is game-specific logic.
 */
#ifndef PPS_SHIM_H
#define PPS_SHIM_H

#include <time.h>
#include <stddef.h>
#include <wchar.h>   /* wint_t, for the wide ctype thunks below */

/* bionic and newlib number the setlocale categories differently -- and in a
 * different order, not merely with an offset. See the .c file. */
char *setlocale_bionic(int category, const char *locale);

/* bionic and newlib number the clocks differently; see the .c file. */
int clock_gettime_bionic(int android_id, struct timespec *tp);

/* newlib has clock_getres only behind _POSIX_TIMERS on some builds. */
int clock_getres_fake(int clk, struct timespec *res);

/* bionic exports these as real functions; newlib makes several of them
 * macros, so the module cannot take their address. */
int isdigit_l_fake(int c, void *loc);
int islower_l_fake(int c, void *loc);
int isupper_l_fake(int c, void *loc);
int isxdigit_l_fake(int c, void *loc);
int tolower_l_fake(int c, void *loc);
int toupper_l_fake(int c, void *loc);

/* mmap/munmap sized for what this game actually does; see the .c file. */
void *pps_mmap(void *addr, size_t len, int prot, int flags, int fd, long offset);
int   pps_munmap(void *addr, size_t len);

/* ------------------------------------------------------------------ */
/* ctype thunks                                                        */
/* ------------------------------------------------------------------ */

/* newlib implements several of these as MACROS over __locale_ctype_ptr[], and
 * a macro has no address to take. The module's import table needs a real
 * function pointer for every one, so each gets a thunk. Cheap, and it removes
 * a whole class of link failure that would otherwise be found one symbol at a
 * time. */
int pps_isalnum(int c);  int pps_isalpha(int c);  int pps_isgraph(int c);
int pps_islower(int c);  int pps_isprint(int c);  int pps_isspace(int c);
int pps_isupper(int c);  int pps_isxdigit(int c);
int pps_tolower(int c);  int pps_toupper(int c);

int pps_iswalpha(unsigned c);  int pps_iswblank(unsigned c);
int pps_iswcntrl(unsigned c);  int pps_iswdigit(unsigned c);
int pps_iswlower(unsigned c);  int pps_iswprint(unsigned c);
int pps_iswpunct(unsigned c);  int pps_iswspace(unsigned c);
int pps_iswupper(unsigned c);  int pps_iswxdigit(unsigned c);
unsigned pps_towlower(unsigned c); unsigned pps_towupper(unsigned c);

/* bionic exports these as real functions taking a locale_t; newlib has the
 * plain forms only. The locale argument is ignored -- the engine only ever
 * passes the C locale. */
long double        pps_strtold_l(const char *s, char **end, void *loc);
long long          pps_strtoll_l(const char *s, char **end, int base, void *loc);
unsigned long long pps_strtoull_l(const char *s, char **end, int base, void *loc);

/* ------------------------------------------------------------------ */
/* Signals -- inert on purpose                                         */
/* ------------------------------------------------------------------ */

/* See the long note in imports.c. The engine installs a crash handler that
 * cannot work here, and these report success so it does not take its
 * "handler refused" path. */
int pps_sigaction(int sig, const void *act, void *old);
int pps_sigaltstack(const void *ss, void *old);
int pps_sigemptyset(void *set);
int pps_sigfillset(void *set);
void *pps_signal(int sig, void *handler);
int pps_raise(int sig);

/* ------------------------------------------------------------------ */
/* Process, terminal and filesystem metadata                           */
/* ------------------------------------------------------------------ */

long pps_ptrace(int request, ...);   /* the engine's anti-debug probe */
int  pps_execl(const char *path, const char *arg, ...);
int  pps_tcgetattr(int fd, void *t);
int  pps_tcsetattr(int fd, int act, const void *t);
int  pps_ioctl(int fd, unsigned long req, ...);
unsigned pps_alarm(unsigned sec);
int  pps_socketpair(int d, int t, int p, int sv[2]);
int  pps_getpagesize(void);
long long pps_lseek64(int fd, long long off, int whence);
int  pps_chmod(const char *p, unsigned m);
int  pps_fchmod(int fd, unsigned m);
int  pps_fchown(int fd, unsigned u, unsigned g);
unsigned pps_umask(unsigned m);
int  pps_utimes(const char *p, const void *tv);
long pps_readlink(const char *p, char *buf, size_t n);
unsigned pps_getuid(void);  unsigned pps_geteuid(void);
unsigned pps_getgid(void);  unsigned pps_getegid(void);
int  pps_dup2(int a, int b);
int  pps_fsync(int fd);
int  pps_ftruncate(int fd, long len);
void pps_openlog(const char *ident, int opt, int fac);
void pps_syslog(int pri, const char *fmt, ...);
void pps_closelog(void);
int  pps_dladdr(const void *addr, void *info);

/* newlib has no rwlocks at all. These are a plain mutex on both paths, which
 * is correct if pessimistic: a reader-writer lock degraded to exclusive is
 * slower, never wrong. The engine uses them around its resource tables, which
 * are not hot. */
int pps_rwlock_init(void *rw, const void *attr);
int pps_rwlock_rdlock(void *rw);
int pps_rwlock_wrlock(void *rw);
int pps_rwlock_unlock(void *rw);
int pps_rwlock_destroy(void *rw);

/* bionic's stdin/stdout/stderr array. NOT a function: the module takes its
 * address and indexes it, so it has to be real storage with the right stride.
 * See pps_shim.c. */

/* Call once, early in main(), before the module is loaded. */

/* mremap has no newlib equivalent; pps_mmap's allocator implements it. */
void *pps_mremap(void *old, size_t oldsz, size_t newsz, int flags, ...);



/* ------------------------------------------------------------------ */
/* ctype thunks                                                        */
/* ------------------------------------------------------------------ */

/* newlib implements much of <ctype.h> and <wctype.h> as macros over a lookup
 * table, and a macro has no address for the module to import. bionic exports
 * all of them as real functions, so each one the module needs gets a thunk. */
int isalnum_fn(int c);  int isalpha_fn(int c);  int isgraph_fn(int c);
int islower_fn(int c);  int isprint_fn(int c);  int isspace_fn(int c);
int isupper_fn(int c);  int isxdigit_fn(int c);
int tolower_fn(int c);  int toupper_fn(int c);

int iswalpha_fn(wint_t c);  int iswblank_fn(wint_t c);
int iswcntrl_fn(wint_t c);  int iswdigit_fn(wint_t c);
int iswlower_fn(wint_t c);  int iswprint_fn(wint_t c);
int iswpunct_fn(wint_t c);  int iswspace_fn(wint_t c);
int iswupper_fn(wint_t c);  int iswxdigit_fn(wint_t c);
wint_t towlower_fn(wint_t c); wint_t towupper_fn(wint_t c);

/* ------------------------------------------------------------------ */
/* deliberately inert                                                  */
/* ------------------------------------------------------------------ */

/* Signals. King's engine installs a crash handler and an alternate signal
 * stack; neither means anything on this console, and letting it take a real
 * handler would put engine code between a fault and libnx's own report. These
 * report success and install nothing. */
int   sigaction_stub(int sig, const void *act, void *old);
int   sigaltstack_stub(const void *ss, void *old);
int   sigemptyset_stub(void *set);
int   sigfillset_stub(void *set);
int   raise_stub(int sig);
/* signal_stub lives in imports_helpers.h, which the reference ports already
 * carry; declaring it again here gave two conflicting prototypes. */

/* Process, ownership and terminal calls. Present in the import table because
 * bionic had them, not because the game does anything with the result. */
long  ptrace_stub(int req, ...);
int   execl_stub(const char *path, const char *arg, ...);
unsigned getuid_stub(void);
unsigned getgid_stub(void);
int   tcgetattr_stub(int fd, void *t);
int   tcsetattr_stub(int fd, int act, const void *t);
int   ioctl_stub(int fd, unsigned long req, ...);
unsigned alarm_stub(unsigned sec);
int   umask_stub(int mask);
int   chmod_stub(const char *p, int mode);
int   fchmod_stub(int fd, int mode);
int   fchown_stub(int fd, unsigned uid, unsigned gid);
int   socketpair_stub(int d, int t, int p, int sv[2]);
void  openlog_stub(const char *ident, int opt, int fac);
void  syslog_stub(int pri, const char *fmt, ...);
void  closelog_stub(void);

/* ------------------------------------------------------------------ */
/* small gaps in newlib                                                */
/* ------------------------------------------------------------------ */

int    getpagesize_fn(void);
long   lseek64_fn(int fd, long off, int whence);
void  *memrchr_fn(const void *s, int c, size_t n);
char  *basename_fn(char *path);




/* ------------------------------------------------------------------ */
/* named here because the reference tables point at their own bodies   */
/* ------------------------------------------------------------------ */

/* gen_imports reuses each reference port's resolution expression wherever it
 * has one. That is correct for the entries backed by libc_shim, and wrong for
 * the handful whose bodies live inside those ports' own files -- sj_exit,
 * sj_gettimeofday and friends. These are this port's equivalents. */
int  pps_gettimeofday(void *tv, void *tz);
unsigned pps_sleep(unsigned seconds);
int  pps_sigsetjmp(void *env, int savemask);
void pps_siglongjmp(void *env, int val) __attribute__((noreturn));
int  pthread_rwlock_init_stub(void **rw, const void *attr);

#endif
