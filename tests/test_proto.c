#include "proto.h"

#include <math.h>
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

#define CHECK_NEAR(a, b, eps)                                                  \
    do {                                                                       \
        checks++;                                                              \
        double _a = (a), _b = (b);                                             \
        if (!(fabs(_a - _b) <= (eps))) {                                       \
            fails++;                                                           \
            printf("FAIL %s:%d: %s (%.6f) != %s (%.6f)\n", __FILE__, __LINE__, \
                   #a, _a, #b, _b);                                            \
        }                                                                      \
    } while (0)

static void roundtrip_ok(const char *json)
{
    pmsg m;
    int rc = proto_parse(json, strlen(json), &m);
    CHECK(rc == 0);
    CHECK(strchr(json, '\n') == NULL);
}

static void test_hb(void)
{
    char *s = proto_encode_hb("peer-1", 1, 612.34, ST_PLAYING, 0.97, 0);
    CHECK(s != NULL);
    roundtrip_ok(s);
    pmsg m;
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.t == MSG_HB);
    CHECK(strcmp(m.id, "peer-1") == 0);
    CHECK(m.has_pos == 1);
    CHECK_NEAR(m.pos, 612.34, 1e-9);
    CHECK(m.state == ST_PLAYING);
    CHECK_NEAR(m.speed, 0.97, 1e-9);
    CHECK(m.joining == 0);
    free(s);

    /* Non-relayed: no "from". Null position must parse as has_pos == 0. */
    s = proto_encode_hb(NULL, 0, 0.0, ST_IDLE, 1.0, 1);
    CHECK(s != NULL);
    CHECK(strstr(s, "\"from\"") == NULL);
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.has_pos == 0);
    CHECK(m.state == ST_IDLE);
    CHECK(m.joining == 1);
    free(s);
}

static void test_intent(void)
{
    char *s = proto_encode_intent("me", ACT_SEEK, 130.0);
    pmsg m;
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.t == MSG_INTENT);
    CHECK(strcmp(m.id, "me") == 0);
    CHECK(m.act == ACT_SEEK);
    CHECK_NEAR(m.pos, 130.0, 1e-9);
    free(s);

    s = proto_encode_intent(NULL, ACT_PAUSE, 120.55);
    CHECK(strstr(s, "\"act\":\"pause\"") != NULL);
    CHECK(strstr(s, "\"from\"") == NULL);
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.act == ACT_PAUSE);
    free(s);

    s = proto_encode_intent(NULL, ACT_RESUME, 1.0);
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.act == ACT_RESUME);
    free(s);
}

static void test_handshake(void)
{
    char *s = proto_encode_hello("id-9", "matteo@laptop", 1);
    pmsg m;
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.t == MSG_HELLO);
    CHECK(m.v == PS_PROTO_VERSION);
    CHECK(strcmp(m.id, "id-9") == 0);
    CHECK(strcmp(m.name, "matteo@laptop") == 0);
    CHECK(m.observer == 1);
    CHECK(strstr(s, "\"impl\":\"playsync2/0.1.0\"") != NULL);
    free(s);

    proto_member mem[2];
    snprintf(mem[0].id, sizeof(mem[0].id), "a");
    snprintf(mem[0].name, sizeof(mem[0].name), "A");
    mem[0].observer = 0;
    snprintf(mem[1].id, sizeof(mem[1].id), "b");
    snprintf(mem[1].name, sizeof(mem[1].name), "B");
    mem[1].observer = 1;
    s = proto_encode_welcome("session-1", "a", mem, 2);
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.t == MSG_WELCOME);
    CHECK(strcmp(m.session, "session-1") == 0);
    CHECK(strcmp(m.you, "a") == 0);
    CHECK(m.nmembers == 2);
    CHECK(m.members[1].observer == 1);
    free(s);

    s = proto_encode_roster(mem, 2);
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.t == MSG_ROSTER);
    CHECK(m.nmembers == 2);
    CHECK(strcmp(m.members[0].name, "A") == 0);
    free(s);
}

static void test_ping_bye_error(void)
{
    pmsg m;
    char *s = proto_encode_ping(123);
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.t == MSG_PING);
    CHECK(m.n == 123);
    free(s);
    s = proto_encode_pong(-7);
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.t == MSG_PONG);
    CHECK(m.n == -7);
    free(s);
    s = proto_encode_bye("mpv_exit");
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.t == MSG_BYE);
    CHECK(strcmp(m.reason, "mpv_exit") == 0);
    free(s);
    s = proto_encode_error(ERR_SESSION_FULL, "full", 1);
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.t == MSG_ERROR);
    CHECK(m.code == ERR_SESSION_FULL);
    CHECK(m.fatal == 1);
    CHECK(strcmp(m.emsg, "full") == 0);
    free(s);
    s = proto_encode_error(ERR_BAD_JSON, "x", 0);
    CHECK(proto_parse(s, strlen(s), &m) == 0);
    CHECK(m.code == ERR_BAD_JSON);
    CHECK(m.fatal == 0);
    free(s);
}

static void test_errors(void)
{
    pmsg m;
    CHECK(proto_parse("not json", 8, &m) == -1);
    CHECK(proto_parse("[1,2,3]", 7, &m) == -1);
    CHECK(proto_parse("{\"t\":\"nope\"}", 12, &m) == -2);
    CHECK(proto_parse("{\"x\":1}", 7, &m) == -2);
    /* Tolerance: extra unknown fields are ignored. */
    const char *extra = "{\"t\":\"ping\",\"n\":4,\"future\":true}";
    CHECK(proto_parse(extra, strlen(extra), &m) == 0);
    CHECK(m.n == 4);
}

/* F02: with every entry at the hello budget, a full welcome/roster fits. */
static void test_member_budget(void)
{
    proto_member m;
    memset(m.id, '\x01', sizeof(m.id) - 1);
    m.id[sizeof(m.id) - 1] = '\0';
    memset(m.name, '\x01', sizeof(m.name) - 1);
    m.name[sizeof(m.name) - 1] = '\0';
    m.observer = 0;
    size_t worst = proto_member_json_len(&m);
    CHECK(worst > PS_MEMBER_JSON_MAX); /* control characters can exceed the budget */

    snprintf(m.id, sizeof(m.id), "a");
    snprintf(m.name, sizeof(m.name), "A");
    CHECK(proto_member_json_len(&m) == strlen("{\"id\":\"a\",\"name\":\"A\",\"observer\":false}"));

    /* Fixed welcome overhead with a worst-case session and "you". */
    char hostile[PS_ID_LEN];
    memset(hostile, '\x01', sizeof(hostile) - 1);
    hostile[sizeof(hostile) - 1] = '\0';
    char *s = proto_encode_welcome(hostile, hostile, NULL, 0);
    CHECK(s != NULL);
    size_t fixed = s ? strlen(s) : PS_MSG_MAX;
    free(s);
    /* n entries plus n-1 commas, plus the framing newline. */
    CHECK(fixed + (size_t)PS_MAX_MEMBERS_CEIL * (PS_MEMBER_JSON_MAX + 1) + 1 <= PS_MSG_MAX);
}

static void test_names(void)
{
    CHECK(strcmp(pstate_name(ST_SEEKING), "seeking") == 0);
    CHECK(pstate_from_name("eof") == ST_EOF);
    CHECK(pstate_from_name("garbage") == ST_IDLE);
    CHECK(strcmp(act_name(ACT_PAUSE), "pause") == 0);
    CHECK(act_from_name("resume") == ACT_RESUME);
    CHECK(act_from_name("bogus") == ACT_NONE);
    CHECK(strcmp(err_name(ERR_TOO_LARGE), "too_large") == 0);
}

int main(void)
{
    test_hb();
    test_intent();
    test_handshake();
    test_ping_bye_error();
    test_member_budget();
    test_errors();
    test_names();
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok", checks, fails);
    return fails ? 1 : 0;
}
