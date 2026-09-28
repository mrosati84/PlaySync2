#ifndef PLAYSYNC2_H
#define PLAYSYNC2_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PS_PROTO_VERSION 1
#define PS_IMPL "playsync2/0.1.0"
#define PS_VERSION_STR "playsync2 0.1.0"

#define PS_DEFAULT_PORT 8765
#define PS_MAX_MEMBERS_DEFAULT 32
#define PS_MAX_MEMBERS_CEIL 64

/* Message cap includes framing; the accumulator grows to this. */
#define PS_MSG_MAX 65536
#define PS_READ_CHUNK 16384

#define PS_HB_INTERVAL 0.5      /* 2 Hz heartbeats */
#define PS_PING_INTERVAL 5.0    /* client ping cadence */
#define PS_LIVENESS_TIMEOUT 15.0 /* server reaps idle connections */
#define PS_PONG_TIMEOUT 15.0    /* client declares server gone */

#define PS_PEER_STALE 5.0       /* heartbeat older than this is ineligible */
#define PS_JOIN_SETTLE 1.0      /* one unbroken second within deadband */
#define PS_HARD_THRESHOLD 1.0   /* hard sync at/above this */
#define PS_HARD_INTERVAL 10.0   /* at most one hard seek per 10 s */
#define PS_NUDGE_INTERVAL 5.0   /* at most one nudge entry per 5 s */
#define PS_MAX_NUDGE 0.05       /* at most 5 % slower */
#define PS_NUDGE_WINDOW 10.0    /* ahead/10 s -> speed delta */

#define PS_ID_LEN 64
#define PS_NAME_LEN 128
#define PS_REASON_LEN 64
#define PS_EMSG_LEN 256

typedef enum {
    ST_IDLE = 0,
    ST_LOADING,
    ST_PLAYING,
    ST_PAUSED,
    ST_BUFFERING,
    ST_SEEKING,
    ST_EOF
} pstate;

typedef enum {
    ACT_NONE = 0,
    ACT_PAUSE,
    ACT_RESUME,
    ACT_SEEK
} intent_act;

typedef enum {
    MSG_UNKNOWN = 0,
    MSG_HELLO,
    MSG_WELCOME,
    MSG_HB,
    MSG_INTENT,
    MSG_PING,
    MSG_PONG,
    MSG_ROSTER,
    MSG_BYE,
    MSG_ERROR
} msg_type;

typedef enum {
    ERR_NONE = 0,
    ERR_UNSUPPORTED_VERSION,
    ERR_BAD_JSON,
    ERR_UNKNOWN_TYPE,
    ERR_TOO_LARGE,
    ERR_DUPLICATE_ID,
    ERR_SESSION_FULL,
    ERR_INTERNAL
} err_code;

/* 0 = quiet (status suppressed), 1 = info, 2 = verbose */
extern int g_log_level;

void log_error(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_trace(const char *fmt, ...);

const char *pstate_name(pstate s);
pstate pstate_from_name(const char *s);
const char *err_name(err_code c);
const char *act_name(intent_act a);
intent_act act_from_name(const char *s);

/*
 * Fill out with a random UUIDv4 (36 chars + NUL). Requires outlen >= 37.
 * Returns 0 on success, -1 when outlen is too small (out[0] is cleared).
 */
int ps_uuid4(char *out, size_t outlen);

#endif /* PLAYSYNC2_H */
