/*
 * LD_PRELOAD fault injector for the recv-error robustness test.
 *
 * After PS_FAIL_RECV_AFTER successful recv() calls, every subsequent call
 * fails with ECONNRESET. The server must treat that as a hard error and close
 * the connection immediately instead of waiting for the liveness reap.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>

static ssize_t (*real_recv)(int, void *, size_t, int) = NULL;

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    if (real_recv == NULL)
        real_recv = (ssize_t (*)(int, void *, size_t, int))dlsym(RTLD_NEXT, "recv");
    const char *env = getenv("PS_FAIL_RECV_AFTER");
    static long calls = 0;
    long limit = env ? strtol(env, NULL, 10) : 0;
    if (calls >= limit) {
        errno = ECONNRESET;
        return -1;
    }
    calls++;
    if (real_recv == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_recv(fd, buf, len, flags);
}
