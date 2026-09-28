#include "net.h"
#include "proto.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;
static int fails = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            fails++;                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                      \
    } while (0)

static void *fail_alloc(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;
    return NULL;
}

/* Item 1: a failed buffer growth is reported and leaves the buffer intact. */
static void test_obuf_oom(void)
{
    obuf o;
    obuf_init(&o);
    CHECK(obuf_append(&o, "abc", 3) == 0);
    size_t before = o.len;

    char big[4096];
    memset(big, 'x', sizeof(big));

    obuf_grow_alloc = fail_alloc;
    /* Grows past the initial 4096-byte capacity -> allocation fails. */
    CHECK(obuf_append(&o, big, sizeof(big)) == -1);
    CHECK(o.len == before);
    CHECK(obuf_append_line(&o, big, sizeof(big)) == -1);
    CHECK(o.len == before);
    /* A size_t-overflowing request is rejected without touching the buffer. */
    CHECK(obuf_append(&o, "x", SIZE_MAX) == -1);
    CHECK(o.len == before);
    obuf_grow_alloc = NULL;

    /* Recovery once the allocator works again, with framing intact. */
    CHECK(obuf_append_line(&o, "hello", 5) == 0);
    CHECK(o.len == before + 6);
    CHECK(o.buf[o.len - 1] == '\n');
    obuf_free(&o);
}

/* Item 2: hostile numbers never trigger an out-of-range double cast. */
static void test_numeric_clamp(void)
{
    pmsg m;
    const char *huge_ping = "{\"t\":\"ping\",\"n\":1e300}";
    CHECK(proto_parse(huge_ping, strlen(huge_ping), &m) == 0);
    CHECK(m.t == MSG_PING);
    CHECK(m.n == LONG_MAX);

    const char *neg_ping = "{\"t\":\"ping\",\"n\":-1e300}";
    CHECK(proto_parse(neg_ping, strlen(neg_ping), &m) == 0);
    CHECK(m.n == LONG_MIN);

    const char *huge_pong = "{\"t\":\"pong\",\"n\":1e300}";
    CHECK(proto_parse(huge_pong, strlen(huge_pong), &m) == 0);
    CHECK(m.n == LONG_MAX);

    const char *huge_v = "{\"t\":\"hello\",\"v\":1e300,\"id\":\"a\",\"name\":\"A\"}";
    CHECK(proto_parse(huge_v, strlen(huge_v), &m) == 0);
    CHECK(m.v == INT_MAX);

    const char *neg_v =
        "{\"t\":\"welcome\",\"v\":-1e300,\"session\":\"s\",\"you\":\"a\"}";
    CHECK(proto_parse(neg_v, strlen(neg_v), &m) == 0);
    CHECK(m.v == INT_MIN);

    /* Ordinary values still pass through unchanged. */
    const char *ok = "{\"t\":\"ping\",\"n\":42}";
    CHECK(proto_parse(ok, strlen(ok), &m) == 0);
    CHECK(m.n == 42);
}

/* Item 3: ps_uuid4 refuses too-small buffers instead of silently doing nothing. */
static void test_uuid_short_buffer(void)
{
    char small[8];
    memset(small, 'Z', sizeof(small));
    CHECK(ps_uuid4(small, sizeof(small)) == -1);
    CHECK(small[0] == '\0');

    char buf[37];
    memset(buf, 0, sizeof(buf));
    CHECK(ps_uuid4(buf, sizeof(buf)) == 0);
    CHECK(strlen(buf) == 36);
    CHECK(buf[8] == '-' && buf[13] == '-' && buf[18] == '-' && buf[23] == '-');

    /* 36 bytes is one short of the required 37 (36 chars + NUL). */
    CHECK(ps_uuid4(buf, 36) == -1);
}

/* Item 4 primitive: a hard recv error surfaces as -1, not would-block. */
static void test_recv_hard_error(void)
{
    char b[8];
    CHECK(net_recv(-1, b, sizeof(b)) == -1);
}

int main(void)
{
    test_obuf_oom();
    test_numeric_clamp();
    test_uuid_short_buffer();
    test_recv_hard_error();
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok", checks, fails);
    return fails ? 1 : 0;
}
