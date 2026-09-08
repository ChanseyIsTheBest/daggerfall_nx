/* pps_io.h -- serialised file I/O.
 *
 * MIT licensed.
 *
 * devkitPro's newlib keeps a process-wide file handle table that is not
 * thread-safe, and this port is not single-threaded: nativeActivateGame spawns
 * loadWhileShowingSplash, which streams .ogg files while the main thread loads
 * textures and writes logs. Every file call the engine makes therefore has to
 * be serialised, and so does every file call this port makes alongside it.
 *
 * These are the only file entry points in imports_osmos.c, and nx_pointer is
 * handed the same lock through its fopen_fn/fclose_fn hooks.
 */
#ifndef PPS_IO_H
#define PPS_IO_H

#include <stdio.h>
#include <stddef.h>

void io_init(void);

FILE  *fopen_locked(const char *path, const char *mode);
int    fclose_locked(FILE *f);
size_t fread_locked(void *p, size_t sz, size_t n, FILE *f);
size_t fwrite_locked(const void *p, size_t sz, size_t n, FILE *f);
int    fseek_locked(FILE *f, long off, int whence);
long   ftell_locked(FILE *f);
int    __open_2_locked(const char *path, int flags);
int    read_locked(int fd, void *buf, size_t n);
int    close_locked(int fd);

/* ------------------------------------------------------------------ */
/* The entry points the module's import table actually binds            */
/* ------------------------------------------------------------------ */

/* These take the file lock and then call libc_shim's *_fake shim, so a call
 * from the module gets BOTH halves it needs: the bionic path translation
 * (absolute /data/data/<pkg> paths rewritten onto the game directory, the
 * "sdmc:" device prefix supplied) and serialisation against the engine's
 * worker threads.
 *
 * Routing imports.c straight at either one alone silently loses the other. */
#include <dirent.h>

FILE  *pps_fopen(const char *path, const char *mode);
size_t pps_fread(void *p, size_t sz, size_t n, FILE *f);
size_t pps_fwrite(const void *p, size_t sz, size_t n, FILE *f);
int    pps_fseek(FILE *f, long off, int whence);
long   pps_ftell(FILE *f);
int    pps_fclose(FILE *f);

int    pps_open(const char *path, int flags, ...);
int    pps_open_2(const char *path, int flags);
long   pps_read(int fd, void *buf, size_t n);
long   pps_write(int fd, const void *buf, size_t n);
int    pps_close(int fd);

void  *pps_opendir(const char *path);
void  *pps_readdir(void *d);
int    pps_closedir(void *d);

#endif
