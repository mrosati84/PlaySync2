#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

void net_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void net_set_nodelay(int fd)
{
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void net_close(int fd)
{
    if (fd >= 0)
        (void)close(fd);
}

static int listen_fd_from(struct addrinfo *ai)
{
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
        return -1;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        close(fd);
        return -1;
    }
    net_set_nonblocking(fd);
    return fd;
}

int net_listen_tcp(const char *bind_addr, int port)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(bind_addr, portstr, &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = listen_fd_from(ai);
        if (fd >= 0)
            break;
    }
    freeaddrinfo(res);
    return fd;
}

int net_connect_tcp(const char *host, int port, double timeout_s)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0)
            continue;
        net_set_nonblocking(s);
        int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fd = s;
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = s;
            pfd.events = POLLOUT;
            int ms = (timeout_s > 0.0) ? (int)(timeout_s * 1000.0) : 0;
            if (poll(&pfd, 1, ms) == 1) {
                int err = 0;
                socklen_t elen = sizeof(err);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
                    fd = s;
                    break;
                }
            }
        }
        close(s);
    }
    freeaddrinfo(res);
    if (fd >= 0)
        net_set_nodelay(fd);
    return fd;
}

int net_unix_listen(const char *path)
{
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(un.sun_path))
        return -1;
    strncpy(un.sun_path, path, sizeof(un.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    (void)unlink(path);
    if (bind(fd, (struct sockaddr *)&un, sizeof(un)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    net_set_nonblocking(fd);
    return fd;
}

int net_unix_connect(const char *path)
{
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(un.sun_path))
        return -1;
    strncpy(un.sun_path, path, sizeof(un.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&un, sizeof(un)) != 0) {
        close(fd);
        return -1;
    }
    net_set_nonblocking(fd);
    return fd;
}

int net_parse_hostport(const char *s, char *host, size_t hostlen, int *port)
{
    const char *colon = strrchr(s, ':');
    if (colon == NULL || colon == s)
        return -1;
    size_t hlen = (size_t)(colon - s);
    if (hlen + 1 > hostlen)
        return -1;
    memcpy(host, s, hlen);
    host[hlen] = '\0';
    char *end = NULL;
    long p = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || p <= 0 || p > 65535)
        return -1;
    *port = (int)p;
    return 0;
}

ssize_t net_recv(int fd, void *buf, size_t n)
{
    for (;;) {
        ssize_t r = recv(fd, buf, n, 0);
        if (r >= 0)
            return r;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -2;
        return -1;
    }
}

void obuf_init(obuf *o)
{
    o->buf = NULL;
    o->len = 0;
    o->cap = 0;
    o->off = 0;
}

void obuf_free(obuf *o)
{
    free(o->buf);
    o->buf = NULL;
    o->len = o->cap = o->off = 0;
}

/* Allocation hook for tests; NULL selects realloc. */
void *(*obuf_grow_alloc)(void *, size_t) = NULL;

static int obuf_grow(obuf *o, size_t extra)
{
    if (o->off == o->len) {
        o->off = 0;
        o->len = 0;
    }
    if (extra > SIZE_MAX - o->len)
        return -1; /* size_t overflow: the request can never be satisfied */
    size_t need = o->len + extra;
    if (need <= o->cap)
        return 0;
    size_t ncap = (o->cap == 0) ? 4096 : o->cap;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    void *(*alloc)(void *, size_t) = obuf_grow_alloc ? obuf_grow_alloc : realloc;
    char *nb = alloc(o->buf, ncap);
    if (nb == NULL)
        return -1;
    o->buf = nb;
    o->cap = ncap;
    return 0;
}

int obuf_append(obuf *o, const void *data, size_t n)
{
    if (obuf_grow(o, n) != 0)
        return -1;
    if (n > 0)
        memcpy(o->buf + o->len, data, n);
    o->len += n;
    return 0;
}

int obuf_append_line(obuf *o, const char *msg, size_t len)
{
    if (len == SIZE_MAX)
        return -1;
    /* Reserve the payload and the framing newline up front so a partial
     * frame is never queued; on failure the buffer is left unchanged. */
    if (obuf_grow(o, len + 1) != 0)
        return -1;
    if (len > 0)
        memcpy(o->buf + o->len, msg, len);
    o->len += len;
    o->buf[o->len++] = '\n';
    return 0;
}

int obuf_pending(const obuf *o)
{
    return o->len > o->off;
}

int obuf_flush(int fd, obuf *o)
{
    while (o->off < o->len) {
        ssize_t w = send(fd, o->buf + o->off, o->len - o->off, MSG_NOSIGNAL);
        if (w > 0) {
            o->off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EINTR))
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 1;
        return -1;
    }
    o->off = 0;
    o->len = 0;
    return 0;
}

void lr_init(lreader *lr)
{
    lr->cap = PS_READ_CHUNK;
    lr->buf = malloc(lr->cap);
    lr->len = 0;
    lr->overflow = 0;
    lr->line = NULL;
    lr->linecap = 0;
    if (lr->buf)
        lr->buf[0] = '\0';
}

void lr_free(lreader *lr)
{
    free(lr->buf);
    free(lr->line);
    lr->buf = NULL;
    lr->line = NULL;
    lr->len = lr->cap = 0;
    lr->linecap = 0;
    lr->overflow = 0;
}

static int lr_reserve(lreader *lr, size_t extra)
{
    if (lr->len + extra + 1 <= lr->cap)
        return 0;
    size_t ncap = lr->cap;
    while (ncap < lr->len + extra + 1)
        ncap *= 2;
    char *nb = realloc(lr->buf, ncap);
    if (nb == NULL)
        return -1;
    lr->buf = nb;
    lr->cap = ncap;
    return 0;
}

ssize_t lr_fill(int fd, lreader *lr)
{
    if (lr->overflow)
        return -1;
    char chunk[PS_READ_CHUNK];
    ssize_t r = net_recv(fd, chunk, sizeof(chunk));
    if (r <= 0)
        return r;
    if (lr_reserve(lr, (size_t)r) != 0)
        return -1;
    memcpy(lr->buf + lr->len, chunk, (size_t)r);
    lr->len += (size_t)r;
    lr->buf[lr->len] = '\0';
    if (lr->len > PS_MSG_MAX)
        lr->overflow = 1;
    return r;
}

int lr_next(lreader *lr, char **line, size_t *len)
{
    if (lr->overflow)
        return -1;
    char *nl = memchr(lr->buf, '\n', lr->len);
    if (nl == NULL) {
        if (lr->len > PS_MSG_MAX)
            return -1;
        return 0;
    }
    size_t i = (size_t)(nl - lr->buf);
    size_t llen = i;
    if (llen > 0 && lr->buf[llen - 1] == '\r')
        llen--;
    if (llen + 1 > lr->linecap) {
        char *nb = realloc(lr->line, llen + 1);
        if (nb == NULL)
            return -1;
        lr->line = nb;
        lr->linecap = llen + 1;
    }
    memcpy(lr->line, lr->buf, llen);
    lr->line[llen] = '\0';
    if (line)
        *line = lr->line;
    if (len)
        *len = llen;
    size_t rest = lr->len - (i + 1);
    if (rest > 0)
        memmove(lr->buf, lr->buf + i + 1, rest);
    lr->len = rest;
    if (lr->buf)
        lr->buf[lr->len] = '\0';
    return 1;
}
