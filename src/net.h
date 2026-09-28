#ifndef PS_NET_H
#define PS_NET_H

#include "playsync2.h"

#include <sys/types.h>

/* Non-blocking TCP listener on bind_addr:port. -1 on error. */
int net_listen_tcp(const char *bind_addr, int port);
/* Non-blocking TCP connect with timeout. -1 on error. */
int net_connect_tcp(const char *host, int port, double timeout_s);
/* Non-blocking UNIX-socket listener (fake-mpv / tests). -1 on error. */
int net_unix_listen(const char *path);
/* Blocking-ish UNIX-socket connect for MPV IPC. -1 on error. */
int net_unix_connect(const char *path);

void net_set_nonblocking(int fd);
void net_set_nodelay(int fd);
void net_close(int fd);
/* Parse "host:port"; returns 0 on success. */
int net_parse_hostport(const char *s, char *host, size_t hostlen, int *port);

/*
 * Non-blocking receive.
 *  > 0  bytes read
 *    0  orderly EOF
 *   -1  error
 *   -2  would block
 */
ssize_t net_recv(int fd, void *buf, size_t n);

/*
 * Output buffer for non-blocking writes. Append complete framed messages;
 * flush when the fd is writable.
 */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t off;
} obuf;

void obuf_init(obuf *o);
void obuf_free(obuf *o);
int obuf_append(obuf *o, const void *data, size_t n);
int obuf_pending(const obuf *o);
/* Drain as much as possible. 0 = drained, 1 = still pending, -1 = error. */
int obuf_flush(int fd, obuf *o);

/*
 * Incremental line reader. Reads through a 16 KiB chunk buffer into an
 * accumulator that grows up to the 64 KiB message cap.
 */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    int overflow;
    char *line;
    size_t linecap;
} lreader;

void lr_init(lreader *lr);
void lr_free(lreader *lr);
/*
 * Read available bytes from fd into the accumulator.
 * Returns bytes read, 0 on EOF, -1 on error, -2 when it would block.
 */
ssize_t lr_fill(int fd, lreader *lr);
/*
 * Extract the next complete line (trailing \n and \r stripped).
 * Returns 1 with *line and *len set (pointer into the accumulator), 0 if no
 * complete line is available, -1 if the accumulator exceeded the cap.
 * The returned pointer is invalidated by any later lr_fill/lr_next call.
 */
int lr_next(lreader *lr, char **line, size_t *len);

#endif
