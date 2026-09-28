#ifndef PS_PROTO_H
#define PS_PROTO_H

#include "playsync2.h"

typedef struct {
    char id[PS_ID_LEN];
    char name[PS_NAME_LEN];
    int observer;
} proto_member;

typedef struct {
    msg_type t;
    /* hello */
    int v;
    char id[PS_ID_LEN];
    char name[PS_NAME_LEN];
    int observer;
    /* welcome */
    char session[PS_ID_LEN];
    char you[PS_ID_LEN];
    /* hb */
    int has_pos;
    double pos;
    pstate state;
    double speed;
    int joining;
    /* intent */
    intent_act act;
    /* ping/pong */
    long n;
    /* bye / error */
    char reason[PS_REASON_LEN];
    err_code code;
    char emsg[PS_EMSG_LEN];
    int fatal;
    /* roster / welcome members */
    proto_member members[PS_MAX_MEMBERS_CEIL];
    int nmembers;
} pmsg;

void proto_init(pmsg *m);

/*
 * Parse one line. Returns:
 *    0  ok (out->t set)
 *   -1  not valid JSON
 *   -2  valid JSON but unknown/missing "t"
 */
int proto_parse(const char *line, size_t len, pmsg *out);

char *proto_encode_hello(const char *id, const char *name, int observer);
char *proto_encode_welcome(const char *session, const char *you,
                           const proto_member *members, int n);
char *proto_encode_roster(const proto_member *members, int n);
char *proto_encode_hb(const char *from, int has_pos, double pos, pstate st,
                      double speed, int joining);
char *proto_encode_intent(const char *from, intent_act act, double pos);
char *proto_encode_ping(long n);
char *proto_encode_pong(long n);
char *proto_encode_bye(const char *reason);
char *proto_encode_error(err_code code, const char *emsg, int fatal);

#endif
