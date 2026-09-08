/* pps_io.c -- see pps_io.h. MIT licensed. */

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "pps_io.h"
#include "libc_shim.h"
#include <stdarg.h>
#include <dirent.h>

/* One recursive lock over all of it.
 *
 * Recursive because the shims call each other -- libc_shim's fopen path can
 * reach open(), and a plain mutex would deadlock on the second acquire. The
 * cost is a lock per file call, which against SD-card latency is nothing. */
static RMutex io_lock;

void io_init(void) { rmutexInit(&io_lock); }

#define IO_ENTER() rmutexLock(&io_lock)
#define IO_LEAVE() rmutexUnlock(&io_lock)

FILE *fopen_locked(const char *path, const char *mode) {
  IO_ENTER();
  FILE *f = fopen(path, mode);
  IO_LEAVE();
  return f;
}

int fclose_locked(FILE *f) {
  IO_ENTER();
  const int r = fclose(f);
  IO_LEAVE();
  return r;
}

size_t fread_locked(void *p, size_t sz, size_t n, FILE *f) {
  IO_ENTER();
  const size_t r = fread(p, sz, n, f);
  IO_LEAVE();
  return r;
}

size_t fwrite_locked(const void *p, size_t sz, size_t n, FILE *f) {
  IO_ENTER();
  const size_t r = fwrite(p, sz, n, f);
  IO_LEAVE();
  return r;
}

int fseek_locked(FILE *f, long off, int whence) {
  IO_ENTER();
  const int r = fseek(f, off, whence);
  IO_LEAVE();
  return r;
}

long ftell_locked(FILE *f) {
  IO_ENTER();
  const long r = ftell(f);
  IO_LEAVE();
  return r;
}

int __open_2_locked(const char *path, int flags) {
  IO_ENTER();
  const int fd = __open_2_fake(path, flags);
  IO_LEAVE();
  return fd;
}

int read_locked(int fd, void *buf, size_t n) {
  IO_ENTER();
  const int r = (int)read(fd, buf, n);
  IO_LEAVE();
  return r;
}

int close_locked(int fd) {
  IO_ENTER();
  const int r = close(fd);
  IO_LEAVE();
  return r;
}


/* ------------------------------------------------------------------ */
/* What imports.c binds                                                */
/* ------------------------------------------------------------------ */

/* Lock, then delegate to the libc_shim shim rather than to newlib directly.
 * See the note in pps_io.h for why both layers are needed. */

FILE *pps_fopen(const char *path, const char *mode) {
  FILE *f;
  IO_ENTER();
  f = fopen_fake(path, mode);
  IO_LEAVE();
  return f;
}

size_t pps_fread(void *p, size_t sz, size_t n, FILE *f) {
  size_t r;
  IO_ENTER();
  r = fread_fake(p, sz, n, f);
  IO_LEAVE();
  return r;
}

size_t pps_fwrite(const void *p, size_t sz, size_t n, FILE *f) {
  size_t r;
  IO_ENTER();
  r = fwrite_fake(p, sz, n, f);
  IO_LEAVE();
  return r;
}

int pps_fseek(FILE *f, long off, int whence) {
  int r;
  IO_ENTER();
  r = fseek_fake(f, off, whence);
  IO_LEAVE();
  return r;
}

long pps_ftell(FILE *f) {
  long r;
  IO_ENTER();
  r = ftell_fake(f);
  IO_LEAVE();
  return r;
}

int pps_fclose(FILE *f) {
  int r;
  IO_ENTER();
  r = fclose_fake(f);
  IO_LEAVE();
  return r;
}

/* open() is variadic in the mode argument. The module only ever passes a mode
 * when O_CREAT is set, but reading it unconditionally is harmless and avoids
 * having to test the flag. */
int pps_open(const char *path, int flags, ...) {
  va_list ap;
  int mode, r;
  va_start(ap, flags);
  mode = va_arg(ap, int);
  va_end(ap);
  IO_ENTER();
  r = open_fake(path, flags, mode);
  IO_LEAVE();
  return r;
}

/* bionic's FORTIFY entry point. There is no newlib equivalent at all. */
int pps_open_2(const char *path, int flags) {
  int r;
  IO_ENTER();
  r = __open_2_fake(path, flags);
  IO_LEAVE();
  return r;
}

long pps_read(int fd, void *buf, size_t n) {
  long r;
  IO_ENTER();
  r = read_fake(fd, buf, n);
  IO_LEAVE();
  return r;
}

long pps_write(int fd, const void *buf, size_t n) {
  long r;
  IO_ENTER();
  r = write_fake(fd, buf, n);
  IO_LEAVE();
  return r;
}

int pps_close(int fd) {
  int r;
  IO_ENTER();
  r = close_fake(fd);
  IO_LEAVE();
  return r;
}

void *pps_opendir(const char *path) {
  void *d;
  IO_ENTER();
  d = opendir(path);
  IO_LEAVE();
  return d;
}

/* readdir returns a pointer into the DIR's own storage, so the lock has to
 * cover the call but cannot cover the caller's use of the result. That is the
 * same contract readdir has everywhere, and the engine reads the entry before
 * calling again. */
void *pps_readdir(void *d) {
  void *e;
  IO_ENTER();
  e = readdir((DIR *)d);
  IO_LEAVE();
  return e;
}

int pps_closedir(void *d) {
  int r;
  IO_ENTER();
  r = closedir((DIR *)d);
  IO_LEAVE();
  return r;
}
