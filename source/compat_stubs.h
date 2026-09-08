/* compat_stubs.h -- satisfies the headers libc_shim.c and so_util.c include.
 *
 * MIT licensed. See LICENSE.
 *
 * libc_shim.c is reused unmodified from the reference ports -- it carries the
 * bionic struct stat translation, the mmap emulation and around 120 correct
 * shims -- but there it is wired into headers belonging to those games:
 *
 *     sj_paths.h  asset_pack.h  bsd_bridge.h  sj_trace.h  fakefd.h  imports.h
 *
 * Papa Pear Saga needs almost none of what they declare. Its assets come through the NDK asset
 * manager rather than libc_shim's APK asset-pack layer -- see pps_assets.c --
 * so the asset_pack hooks stay inert here. Sockets are stubbed deliberately
 * rather than absently: see the long note in compat_stubs.c.
 *
 * fakefd IS real in this port, unlike in Osmos. The engine imports pipe() and
 * socketpair(), and its loader threads use a pipe to wake each other, so
 * fakefd.c is compiled in and its declarations come from fakefd.h.
 *
 * So those headers are one-line forwarders to this one and compat_stubs.c
 * gives every symbol a trivial body, letting libc_shim.c drop in untouched --
 * which is what allows fixes to be pulled back from upstream.
 *
 * Two things this file learned the hard way, both recorded so they are not
 * relearned:
 *
 *   1. The prototypes are copied verbatim from the reference headers, not
 *      retyped. An earlier draft guessed and produced eight silent signature
 *      mismatches: asset_pack_dir_is takes a directory handle rather than a
 *      path, and asset_pack_fstat_fd takes four arguments rather than two.
 *
 *   2. The list of symbols is derived from the undefined symbols of the built
 *      libc_shim.o, not from reading its includes. Reading the includes missed
 *      asset_pack_read_all_path, nx_socket and firebase_stub_lookup, none of
 *      which would have surfaced until the link.
 *
 * NOTE: nx_addr_readable and managed_path are deliberately absent. The first
 * is static inside libc_shim.c; the second is declared by libc_shim.h and
 * defined in libc_shim.c. Declaring either here would be a duplicate.
 */
#ifndef PPS_COMPAT_STUBS_H
#define PPS_COMPAT_STUBS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* libc_shim.c's mmap emulation consults these.
 *
 * g_mmap_big_align MUST BE A NON-ZERO POWER OF TWO. It is not an "alignment
 * hint" that can be switched off, which is what an earlier version of this
 * comment claimed, and setting it to 0 hung the port for six test cycles:
 *
 *     libc_shim.c:1248   const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;
 *     libc_shim.c:1251   for (size_t i = 0; i + need <= mmap_pages; i += step)
 *
 * With big_align 0 the step is 0 and the scan never advances -- an infinite
 * loop with no syscall, no allocation and no I/O, which is why every counter
 * froze and the console sat at 100% CPU. And because MMAP_BIG_THRESH is
 * #defined to the same value, `len >= MMAP_BIG_THRESH` was `len >= 0`, so
 * *every* mmap took that path. The first one never returned.
 *
 * The reference port derives these from an __libnx_initheap override. This
 * port deliberately does not override initheap -- libnx's default claims all
 * available memory via svcSetHeapSize, which is both more memory and less to
 * go wrong -- so pps_mmap_arena_init() carves the arena out of that heap
 * instead. */
extern void  *g_mmap_arena_base;   /* granule-aligned arena, or NULL          */
extern size_t g_mmap_arena_size;
extern size_t g_mmap_big_align;    /* NON-ZERO power of two; used as a divisor */
extern int    g_overcommit;        /* 0 = do not pretend allocations succeed  */

/* Allocate the arena and publish the three globals above. Call once, early in
 * main(), before anything can reach mmap. */
void pps_mmap_arena_init(void);

int asset_pack_close_fd(int fd);
int asset_pack_closedir_path(void *dir);
int asset_pack_dir_is(const void *dir);
int asset_pack_fd_is(int fd);
int asset_pack_fstat_fd(int fd, uint64_t *size, uint64_t *ino, int *directory);
long asset_pack_lseek_fd(int fd, long offset, int whence);
int asset_pack_open_path(const char *path);
long asset_pack_pread_fd(int fd, void *buffer, size_t count, long offset);
int asset_pack_read_all_path(const char *path, void **data, size_t *size);
long asset_pack_read_fd(int fd, void *buffer, size_t count);
const char *asset_pack_readdir_path(void *dir, uint8_t *type, uint64_t *ino);
int asset_pack_stat_path_info(const char *path, uint64_t *size, uint64_t *ino, int *directory);
uintptr_t dynlib_find_export(const char *name);
void *firebase_stub_lookup(const char *name);
int nx_accept(int fd, void *addr, void *addrlen);
int nx_bind(int fd, const void *addr, unsigned addrlen);
int nx_connect(int fd, const void *addr, unsigned addrlen);
void nx_freeaddrinfo(void *res);
int nx_getaddrinfo(const char *node, const char *service, const void *hints, void **res);
int nx_getnameinfo(const void *addr, unsigned addrlen, char *host, unsigned hostlen, char *serv, unsigned servlen, int flags);
int nx_getpeername(int fd, void *addr, void *addrlen);
int nx_getsockname(int fd, void *addr, void *addrlen);
int nx_getsockopt(int fd, int level, int optname, void *optval, void *optlen);
int nx_listen(int fd, int backlog);
int nx_poll(void *fds, unsigned long nfds, int timeout);
long nx_recv(int fd, void *buf, size_t len, int flags);
long nx_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, void *addrlen);
int nx_select(int nfds, void *rd, void *wr, void *ex, void *timeout);
long nx_send(int fd, const void *buf, size_t len, int flags);
long nx_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen);
int nx_setsockopt(int fd, int level, int optname, const void *optval, unsigned optlen);
int nx_shutdown(int fd, int how);
int nx_socket(int domain, int type, int protocol);
const char *sj_home(void);
void sj_mark(const char *what);
void sj_trace_open(const char *path, int ok);

#endif
