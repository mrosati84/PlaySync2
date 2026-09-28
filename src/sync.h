#ifndef PS_SYNC_H
#define PS_SYNC_H

#include "playsync2.h"

/* The min-wins core. Pure: no sockets, no globals, `now` is injected. */

typedef struct {
    char id[PS_ID_LEN];
    int has_pos;
    double pos0;      /* last reported position (seconds) */
    double speed;
    pstate state;
    int joining;
    int observer;
    double t_recv;    /* LOCAL monotonic receipt time */
} sync_peer;

enum {
    SYNC_GS_UNKNOWN = -1,
    SYNC_GS_PAUSED = 0,
    SYNC_GS_PLAYING = 1
};

int sync_peer_fresh(const sync_peer *p, double now);
double sync_extrapolate(const sync_peer *p, double now);
/* Lowest extrapolated *playing* position, or -1.0 when undefined. */
double sync_group_min(const sync_peer *peers, int n, double now);
/* Lowest raw position of the paused/eof bucket, or -1.0 when undefined. */
double sync_group_static(const sync_peer *peers, int n, double now);
int sync_group_state(const sync_peer *peers, int n, double now);

double sync_deadband(double rtt_self);
double sync_nudge_speed(double ahead);

typedef enum {
    SYNC_CMD_NONE = 0,
    SYNC_CMD_SEEK,
    SYNC_CMD_SPEED,
    SYNC_CMD_PAUSE
} sync_cmd_kind;

typedef struct {
    sync_cmd_kind kind;
    double value;
} sync_cmd;

typedef struct {
    int joining;
    int was_joining;
    double within_since;         /* join-convergence clock */
    double last_hard;            /* -inf initially */
    double last_nudge_transition;
    double speed_applied;        /* engine-owned speed */
    int nudging;
    double hold_until;           /* suppress drift correction after an intent */
} sync_state;

typedef struct {
    double now;
    double rtt_self;
    pstate state;
    int has_pos;
    double pos;
    int observer;
    const sync_peer *peers;
    int npeers;
} sync_input;

void sync_state_init(sync_state *ss);
void sync_begin_join(sync_state *ss, double now);
/* Suppress drift correction until now+seconds (an explicit intent re-bases). */
void sync_hold(sync_state *ss, double now, double seconds);

#define SYNC_MAX_CMDS 4
int sync_tick(sync_state *ss, const sync_input *in, sync_cmd *cmds, int maxcmds);

#endif
