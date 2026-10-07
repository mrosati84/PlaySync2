#include "net.h"
#include "proto.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

/* F01: unsent output is bounded per buffer; a refused frame changes nothing. */
static void test_obuf_limit(void)
{
    obuf o;
    obuf_init(&o);
    CHECK(o.limit == PS_OBUF_MAX);
    o.limit = 8192;
    char frame[1000];
    memset(frame, 'f', sizeof(frame));
    int r, n = 0;
    while ((r = obuf_append_line(&o, frame, sizeof(frame))) == 0)
        n++;
    CHECK(r == -2);
    CHECK(n == 8); /* 8 * 1001 fits in 8192, a ninth would not */
    CHECK(obuf_pending_bytes(&o) == 8 * 1001);
    CHECK(o.cap <= o.limit);

    /* Once the consumer catches up the space is usable again, and the sent
     * prefix is compacted away instead of growing the allocation. */
    size_t cap = o.cap;
    o.off = 5 * 1001;
    CHECK(obuf_append_line(&o, frame, sizeof(frame)) == 0);
    CHECK(o.cap == cap);
    CHECK(o.off == 0);
    CHECK(obuf_pending_bytes(&o) == 4 * 1001);
    CHECK(o.buf[o.len - 1] == '\n');
    obuf_free(&o);
    CHECK(obuf_total_allocated() == 0);
}

/* F01: all output buffers together never exceed the process-wide cap. */
static void test_obuf_total_cap(void)
{
    enum { NB = 64 };
    static obuf bufs[NB];
    static char chunk[256 << 10];
    memset(chunk, 'c', sizeof(chunk));
    int refused = 0;
    for (int i = 0; i < NB; i++) {
        obuf_init(&bufs[i]);
        for (int k = 0; k < 4; k++)
            if (obuf_append(&bufs[i], chunk, sizeof(chunk)) == -2)
                refused = 1;
        CHECK(obuf_total_allocated() <= PS_OBUF_TOTAL_MAX);
    }
    CHECK(refused);
    for (int i = 0; i < NB; i++)
        obuf_free(&bufs[i]);
    CHECK(obuf_total_allocated() == 0);
}

/* F01: a drained buffer does not keep its high-water allocation. */
static void test_obuf_release_after_drain(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    net_set_nonblocking(sv[0]);
    obuf o;
    obuf_init(&o);
    static char chunk[200 << 10];
    memset(chunk, 'd', sizeof(chunk));
    CHECK(obuf_append(&o, chunk, sizeof(chunk)) == 0);
    CHECK(o.cap > PS_OBUF_KEEP);
    char sink[65536];
    int r, spins = 0;
    while ((r = obuf_flush(sv[0], &o)) == 1 && spins++ < 1000)
        (void)read(sv[1], sink, sizeof(sink));
    CHECK(r == 0);
    CHECK(o.cap == 0 && o.buf == NULL);
    CHECK(obuf_total_allocated() == 0);
    obuf_free(&o);
    close(sv[0]);
    close(sv[1]);
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
    test_obuf_limit();
    test_obuf_total_cap();
    test_obuf_release_after_drain();
    test_numeric_clamp();
    test_uuid_short_buffer();
    test_recv_hard_error();
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok", checks, fails);
    return fails ? 1 : 0;
}
