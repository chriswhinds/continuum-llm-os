/* sock_util.h -- small POSIX socket helpers shared by lib/wire and every
 * daemon that speaks TCP (node-to-node) or Unix domain sockets (local IPC,
 * e.g. api-gatewayd -> schedulerd, per ARCH-002 §05). */
#ifndef CONTINUUM_SOCK_UTIL_H
#define CONTINUUM_SOCK_UTIL_H

#include <stdint.h>

int sock_set_nonblocking(int fd);
int sock_set_reuseaddr(int fd);
/* Disables Nagle's algorithm -- Continuum's wire frames are already sized
 * and corked deliberately by the caller (ARCH-002 §07), so TCP's own
 * coalescing only adds latency. */
int sock_set_nodelay(int fd);

/* bind_addr may be NULL/"" for INADDR_ANY. Returns a listening, non-blocking
 * fd, or -1 on error (errno set). */
int sock_tcp_listen(const char *bind_addr, uint16_t port, int backlog);

/* Blocking connect (simple and sufficient for the reference build's control
 * paths; ARCH-002 does not require non-blocking connect anywhere). Returns
 * a connected fd, or -1 on error. */
int sock_tcp_connect(const char *host, uint16_t port);

int sock_unix_listen(const char *path, int backlog);
int sock_unix_connect(const char *path);

#endif
