/* imports_helpers.h -- see imports_helpers.c. MIT licensed. */
#ifndef OSMOS_IMPORTS_HELPERS_H
#define OSMOS_IMPORTS_HELPERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

/* stack protector / cxxabi */
extern uint64_t __stack_chk_guard_fake;
void __stack_chk_fail_fake(void);
int  __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso);
void __cxa_finalize_fake(void *dso);

/* pthread: bionic's opaque types are reinterpreted as pointer slots and
 * lazily backed with real newlib objects. See imports_helpers.c. */
int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr);
int pthread_mutex_destroy_fake(pthread_mutex_t **uid);
int pthread_mutex_lock_fake(pthread_mutex_t **uid);
int pthread_mutex_trylock_fake(pthread_mutex_t **uid);
int pthread_mutex_unlock_fake(pthread_mutex_t **uid);
int pthread_mutexattr_init_fake(int *attr);
int pthread_mutexattr_settype_fake(int *attr, int type);
int pthread_mutexattr_destroy_fake(int *attr);

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *attr);
int pthread_cond_destroy_fake(pthread_cond_t **cnd);
int pthread_cond_signal_fake(pthread_cond_t **cnd);
int pthread_cond_broadcast_fake(pthread_cond_t **cnd);
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **uid);
int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **uid,
                                const struct timespec *t);

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void));
int pthread_create_fake(pthread_t *thread, const void *bionic_attr,
                        void *entry, void *arg);
int pthread_join_fake(pthread_t thread, void **retval);

/* bionic's pthread_key_t is an unsigned int, and devkitA64 backs real keys
 * with a pool of only ~16 libnx TLS slots. These multiplex many bionic keys
 * over one real key, which is why the types must match the definitions in
 * imports_helpers.c exactly rather than being approximated as int. */
int   pthread_key_create_fake(unsigned int *key, void (*destructor)(void *));
int   pthread_key_delete_fake(unsigned int key);
int   pthread_setspecific_fake(unsigned int key, const void *value);
void *pthread_getspecific_fake(unsigned int key);

/* null-tolerant string wrappers: the engine passes NULL where bionic
 * tolerated it and newlib faults. */
int     z_strcmp(const char *a, const char *b);
int     z_strncmp(const char *a, const char *b, size_t n);
char   *z_strstr(const char *h, const char *n);
char   *z_strchr(const char *s, int c);
char   *z_strrchr(const char *s, int c);
size_t  z_strlen(const char *s);

/* newlib's errno accessor, which bionic spells __errno. */
extern int *__errno(void);

/* exit() must neither kill the process -- that skips the engine's
 * save-on-exit write -- nor return, since it is declared noreturn and there is
 * nothing sane to return into. pps_exit does the only correct third thing:
 * flag the quit so the frame loop unwinds normally, then park. */
void pps_exit(int code);

/* small stubs */
int  ret0_i(void);
int  signal_stub(int sig, void *handler);
long sysconf_pass(int name);
void sincos_fake(double x, double *s, double *c);
/* sincosf_fake is declared by libc_shim.h -- do not redeclare it here. */

#endif
