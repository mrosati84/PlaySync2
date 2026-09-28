#include "sync.h"

#include <math.h>

static double clampd(double v, double lo, double hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

int sync_peer_fresh(const sync_peer *p, double now)
{
    if (p->observer || p->joining || !p->has_pos)
        return 0;
    return (now - p->t_recv) <= PS_PEER_STALE;
}

double sync_extrapolate(const sync_peer *p, double now)
{
    return p->pos0 + p->speed * (now - p->t_recv);
}

double sync_group_min(const sync_peer *peers, int n, double now)
{
    double m = -1.0;
    for (int i = 0; i < n; i++) {
        const sync_peer *p = &peers[i];
        if (!sync_peer_fresh(p, now) || p->state != ST_PLAYING)
            continue;
        double cand = sync_extrapolate(p, now);
        if (cand < 0.0)
            continue;
        if (m < 0.0 || cand < m)
            m = cand;
    }
    return m;
}

double sync_group_static(const sync_peer *peers, int n, double now)
{
    double s = -1.0;
    for (int i = 0; i < n; i++) {
        const sync_peer *p = &peers[i];
        if (!sync_peer_fresh(p, now))
            continue;
        if (p->state != ST_PAUSED && p->state != ST_EOF)
            continue;
        if (p->pos0 < 0.0)
            continue;
        if (s < 0.0 || p->pos0 < s)
            s = p->pos0;
    }
    return s;
}

int sync_group_state(const sync_peer *peers, int n, double now)
{
    int playing = 0, paused = 0;
    for (int i = 0; i < n; i++) {
        const sync_peer *p = &peers[i];
        if (!sync_peer_fresh(p, now))
            continue;
        if (p->state == ST_PLAYING)
            playing = 1;
        else if (p->state == ST_PAUSED || p->state == ST_EOF)
            paused = 1;
    }
    if (playing)
        return SYNC_GS_PLAYING;
    if (paused)
        return SYNC_GS_PAUSED;
    return SYNC_GS_UNKNOWN;
}

double sync_deadband(double rtt_self)
{
    double d = rtt_self + 0.050;
    if (d < 0.150)
        d = 0.150;
    if (d > 1.000)
        d = 1.000;
    return d;
}

double sync_nudge_speed(double ahead)
{
    return 1.0 - clampd(ahead / PS_NUDGE_WINDOW, 0.01, PS_MAX_NUDGE);
}

void sync_state_init(sync_state *ss)
{
    ss->joining = 1;
    ss->was_joining = 0;
    ss->within_since = 0.0;
    ss->last_hard = -1e18;
    ss->last_nudge_transition = -1e18;
    ss->speed_applied = 1.0;
    ss->nudging = 0;
    ss->hold_until = -1e18;
}

void sync_begin_join(sync_state *ss, double now)
{
    ss->joining = 1;
    ss->was_joining = 1;
    ss->within_since = now;
}

void sync_hold(sync_state *ss, double now, double seconds)
{
    double until = now + seconds;
    if (until > ss->hold_until)
        ss->hold_until = until;
}

static void emit(sync_cmd *cmds, int *n, int max, sync_cmd_kind kind, double v)
{
    if (*n >= max)
        return;
    cmds[*n].kind = kind;
    cmds[*n].value = v;
    (*n)++;
}

static void set_speed(sync_state *ss, sync_cmd *cmds, int *n, int max, double target)
{
    if (target > 0.999 && target < 1.001)
        target = 1.0;
    if (fabs(target - ss->speed_applied) < 1e-9)
        return;
    emit(cmds, n, max, SYNC_CMD_SPEED, target);
    ss->speed_applied = target;
    ss->nudging = (target < 0.999);
}

static void restore_speed(sync_state *ss, sync_cmd *cmds, int *n, int max)
{
    if (ss->nudging)
        set_speed(ss, cmds, n, max, 1.0);
}

int sync_tick(sync_state *ss, const sync_input *in, sync_cmd *cmds, int maxcmds)
{
    int n = 0;
    double now = in->now;

    if (in->observer || !in->has_pos)
        return 0;

    double deadband = sync_deadband(in->rtt_self);

    if (ss->joining) {
        if (!ss->was_joining)
            ss->within_since = now;
        ss->was_joining = 1;

        int gs = sync_group_state(in->peers, in->npeers, now);
        if (gs == SYNC_GS_PLAYING) {
            double m = sync_group_min(in->peers, in->npeers, now);
            if (m < 0.0) {
                ss->within_since = now;
            } else if (fabs(in->pos - m) > deadband) {
                emit(cmds, &n, maxcmds, SYNC_CMD_SEEK, m);
                ss->within_since = now;
            } else if (now - ss->within_since >= PS_JOIN_SETTLE) {
                ss->joining = 0;
            }
            /* adopt the group's play state */
            emit(cmds, &n, maxcmds, SYNC_CMD_PAUSE, 0.0);
        } else if (gs == SYNC_GS_PAUSED) {
            double s = sync_group_static(in->peers, in->npeers, now);
            if (s >= 0.0 && fabs(in->pos - s) > deadband) {
                emit(cmds, &n, maxcmds, SYNC_CMD_SEEK, s);
                ss->within_since = now;
            } else if (now - ss->within_since >= PS_JOIN_SETTLE) {
                ss->joining = 0;
            }
            emit(cmds, &n, maxcmds, SYNC_CMD_PAUSE, 1.0);
        } else {
            ss->joining = 0;
        }
        return n;
    }
    ss->was_joining = 0;

    if (in->state != ST_PLAYING) {
        restore_speed(ss, cmds, &n, maxcmds);
        return n;
    }

    /* An explicit intent re-bases the group: do not let stale peer
     * heartbeats drag this member back before they adopt the intent. */
    if (now < ss->hold_until) {
        restore_speed(ss, cmds, &n, maxcmds);
        return n;
    }

    double m = sync_group_min(in->peers, in->npeers, now);
    if (m < 0.0) {
        restore_speed(ss, cmds, &n, maxcmds);
        return n;
    }

    double ahead = in->pos - m;

    if (ahead >= PS_HARD_THRESHOLD && (now - ss->last_hard) >= PS_HARD_INTERVAL) {
        emit(cmds, &n, maxcmds, SYNC_CMD_SEEK, m);
        ss->last_hard = now;
        set_speed(ss, cmds, &n, maxcmds, 1.0);
    } else if (ahead > deadband && (now - ss->last_nudge_transition) >= PS_NUDGE_INTERVAL) {
        set_speed(ss, cmds, &n, maxcmds, sync_nudge_speed(ahead));
        ss->last_nudge_transition = now;
    } else if (ahead <= deadband / 2.0) {
        set_speed(ss, cmds, &n, maxcmds, 1.0);
    }

    return n;
}
