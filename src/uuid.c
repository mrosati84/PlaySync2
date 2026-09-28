#include "playsync2.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int ps_uuid4(char *out, size_t outlen)
{
    if (out == NULL || outlen < 37) {
        /* Never leave the caller's buffer silently uninitialised. */
        if (out != NULL && outlen > 0)
            out[0] = '\0';
        return -1;
    }
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, b, sizeof(b));
        close(fd);
        if (r != (ssize_t)sizeof(b))
            fd = -1;
    }
    if (fd < 0) {
        for (size_t i = 0; i < sizeof(b); i++)
            b[i] = (unsigned char)(rand() & 0xff);
    }
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    snprintf(out, outlen,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
             b[11], b[12], b[13], b[14], b[15]);
    return 0;
}
