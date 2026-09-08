#ifndef FAKEFD_H
#define FAKEFD_H

#include <stdint.h>

int fakefd_is_fake(int fd);
/* Bytes buffered on a pipe read end (0 if none / not a read end). */
int fakefd_readable(int fd);
/* Wait for readability. timeout_ns < 0 blocks forever, 0 polls. Returns 1/0. */
int fakefd_wait_readable(int fd, int64_t timeout_ns);
int fakefd_pipe(int fds[2]);
long fakefd_read(int fd, void *buffer, unsigned long size);
long fakefd_write(int fd, const void *buffer, unsigned long size);
int fakefd_close(int fd);

#endif
