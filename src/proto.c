#include "proto.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static const char *const STATE_NAMES[] = {
    "idle", "loading", "playing", "paused", "buffering", "seeking", "eof"
};

const char *pstate_name(pstate s)
{
    if ((int)s < 0 || (int)s > ST_EOF)
        return "idle";
    return STATE_NAMES[(int)s];
}

pstate pstate_from_name(const char *s)
{
    for (int i = 0; i <= (int)ST_EOF; i++) {
        if (s && strcmp(s, STATE_NAMES[i]) == 0)
            return (pstate)i;
    }
    return ST_IDLE;
}

const char *err_name(err_code c)
{
    switch (c) {
    case ERR_UNSUPPORTED_VERSION: return "unsupported_version";
    case ERR_BAD_JSON: return "bad_json";
    case ERR_UNKNOWN_TYPE: return "unknown_type";
    case ERR_TOO_LARGE: return "too_large";
    case ERR_DUPLICATE_ID: return "duplicate_id";
    case ERR_SESSION_FULL: return "session_full";
    case ERR_INTERNAL: return "internal";
    default: return "internal";
    }
}

const char *act_name(intent_act a)
{
    switch (a) {
    case ACT_PAUSE: return "pause";
    case ACT_RESUME: return "resume";
    case ACT_SEEK: return "seek";
    default: return NULL;
    }
}

intent_act act_from_name(const char *s)
{
    if (!s)
        return ACT_NONE;
    if (strcmp(s, "pause") == 0)
        return ACT_PAUSE;
    if (strcmp(s, "resume") == 0)
        return ACT_RESUME;
    if (strcmp(s, "seek") == 0)
        return ACT_SEEK;
    return ACT_NONE;
}

void proto_init(pmsg *m)
{
    memset(m, 0, sizeof(*m));
    m->speed = 1.0;
}

static void copy_str(char *dst, size_t dstlen, const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring != NULL)
        snprintf(dst, dstlen, "%s", it->valuestring);
}

static int read_bool(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsTrue(it) ? 1 : 0;
}

static int read_number(const cJSON *obj, const char *key, double *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        *out = it->valuedouble;
        return 1;
    }
    return 0;
}

static int read_string(const cJSON *obj, const char *key, char *out, size_t outlen)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        snprintf(out, outlen, "%s", it->valuestring);
        return 1;
    }
    return 0;
}

/* Append root's "members" array to out, ignoring a missing/non-array field. */
static void parse_members(const cJSON *root, pmsg *out)
{
    const cJSON *members = cJSON_GetObjectItemCaseSensitive(root, "members");
    if (!cJSON_IsArray(members))
        return;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, members) {
        if (out->nmembers >= PS_MAX_MEMBERS_CEIL)
            break;
        proto_member *m = &out->members[out->nmembers];
        copy_str(m->id, sizeof(m->id), it, "id");
        copy_str(m->name, sizeof(m->name), it, "name");
        m->observer = read_bool(it, "observer");
        out->nmembers++;
    }
}

int proto_parse(const char *line, size_t len, pmsg *out)
{
    proto_init(out);
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (root == NULL)
        return -1;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    char t[32];
    t[0] = '\0';
    if (!read_string(root, "t", t, sizeof(t))) {
        cJSON_Delete(root);
        return -2;
    }

    if (strcmp(t, "hello") == 0) {
        out->t = MSG_HELLO;
        double v = 0;
        if (read_number(root, "v", &v))
            out->v = (int)v;
        copy_str(out->id, sizeof(out->id), root, "id");
        copy_str(out->name, sizeof(out->name), root, "name");
        out->observer = read_bool(root, "observer");
    } else if (strcmp(t, "welcome") == 0) {
        out->t = MSG_WELCOME;
        double v = 0;
        if (read_number(root, "v", &v))
            out->v = (int)v;
        copy_str(out->session, sizeof(out->session), root, "session");
        copy_str(out->you, sizeof(out->you), root, "you");
        parse_members(root, out);
    } else if (strcmp(t, "roster") == 0) {
        out->t = MSG_ROSTER;
        parse_members(root, out);
    } else if (strcmp(t, "hb") == 0) {
        out->t = MSG_HB;
        copy_str(out->id, sizeof(out->id), root, "from");
        double pos = 0;
        out->has_pos = read_number(root, "pos", &pos);
        out->pos = pos;
        char st[32];
        st[0] = '\0';
        read_string(root, "state", st, sizeof(st));
        out->state = pstate_from_name(st);
        double sp = 1.0;
        read_number(root, "speed", &sp);
        out->speed = sp;
        out->joining = read_bool(root, "joining");
    } else if (strcmp(t, "intent") == 0) {
        out->t = MSG_INTENT;
        copy_str(out->id, sizeof(out->id), root, "from");
        char act[32];
        act[0] = '\0';
        read_string(root, "act", act, sizeof(act));
        out->act = act_from_name(act);
        double pos = 0;
        out->has_pos = read_number(root, "pos", &pos);
        out->pos = pos;
    } else if (strcmp(t, "ping") == 0) {
        out->t = MSG_PING;
        double n = 0;
        read_number(root, "n", &n);
        out->n = (long)n;
    } else if (strcmp(t, "pong") == 0) {
        out->t = MSG_PONG;
        double n = 0;
        read_number(root, "n", &n);
        out->n = (long)n;
    } else if (strcmp(t, "bye") == 0) {
        out->t = MSG_BYE;
        read_string(root, "reason", out->reason, sizeof(out->reason));
    } else if (strcmp(t, "error") == 0) {
        out->t = MSG_ERROR;
        char code[64];
        code[0] = '\0';
        read_string(root, "code", code, sizeof(code));
        out->code = ERR_INTERNAL;
        for (int i = 1; i <= (int)ERR_INTERNAL; i++) {
            if (strcmp(code, err_name((err_code)i)) == 0) {
                out->code = (err_code)i;
                break;
            }
        }
        read_string(root, "msg", out->emsg, sizeof(out->emsg));
        out->fatal = read_bool(root, "fatal");
    } else {
        cJSON_Delete(root);
        return -2;
    }
    cJSON_Delete(root);
    return 0;
}

static cJSON *add_hb(cJSON *o, const char *from, int has_pos, double pos,
                     pstate st, double speed, int joining, int relayed)
{
    cJSON_AddStringToObject(o, "t", "hb");
    if (relayed && from)
        cJSON_AddStringToObject(o, "from", from);
    if (has_pos)
        cJSON_AddNumberToObject(o, "pos", pos);
    else
        cJSON_AddNullToObject(o, "pos");
    cJSON_AddStringToObject(o, "state", pstate_name(st));
    cJSON_AddNumberToObject(o, "speed", speed);
    cJSON_AddBoolToObject(o, "joining", joining ? 1 : 0);
    return o;
}

char *proto_encode_hello(const char *id, const char *name, int observer)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "t", "hello");
    cJSON_AddNumberToObject(o, "v", PS_PROTO_VERSION);
    cJSON_AddStringToObject(o, "impl", PS_IMPL);
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddBoolToObject(o, "observer", observer ? 1 : 0);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static cJSON *members_array(const proto_member *members, int n)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;
    for (int i = 0; i < n; i++) {
        cJSON *m = cJSON_CreateObject();
        if (!m)
            break;
        cJSON_AddStringToObject(m, "id", members[i].id);
        cJSON_AddStringToObject(m, "name", members[i].name);
        cJSON_AddBoolToObject(m, "observer", members[i].observer ? 1 : 0);
        cJSON_AddItemToArray(arr, m);
    }
    return arr;
}

char *proto_encode_welcome(const char *session, const char *you,
                           const proto_member *members, int n)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "t", "welcome");
    cJSON_AddNumberToObject(o, "v", PS_PROTO_VERSION);
    cJSON_AddStringToObject(o, "session", session);
    cJSON_AddStringToObject(o, "you", you);
    cJSON *arr = members_array(members, n);
    if (arr)
        cJSON_AddItemToObject(o, "members", arr);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

char *proto_encode_roster(const proto_member *members, int n)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "t", "roster");
    cJSON *arr = members_array(members, n);
    if (arr)
        cJSON_AddItemToObject(o, "members", arr);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

char *proto_encode_hb(const char *from, int has_pos, double pos, pstate st,
                      double speed, int joining)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    add_hb(o, from, has_pos, pos, st, speed, joining, from != NULL);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

char *proto_encode_intent(const char *from, intent_act act, double pos)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "t", "intent");
    if (from)
        cJSON_AddStringToObject(o, "from", from);
    cJSON_AddStringToObject(o, "act", act_name(act));
    cJSON_AddNumberToObject(o, "pos", pos);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

char *proto_encode_ping(long n)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "t", "ping");
    cJSON_AddNumberToObject(o, "n", (double)n);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

char *proto_encode_pong(long n)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "t", "pong");
    cJSON_AddNumberToObject(o, "n", (double)n);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

char *proto_encode_bye(const char *reason)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "t", "bye");
    cJSON_AddStringToObject(o, "reason", reason ? reason : "shutdown");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

char *proto_encode_error(err_code code, const char *emsg, int fatal)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "t", "error");
    cJSON_AddStringToObject(o, "code", err_name(code));
    cJSON_AddStringToObject(o, "msg", emsg ? emsg : "");
    cJSON_AddBoolToObject(o, "fatal", fatal ? 1 : 0);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}
