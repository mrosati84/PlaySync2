#include "sync.h"

#include <math.h>
#include <stdio.h>
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

static sync_peer mkpeer(const char *id, pstate st, int has_pos, double pos,
                        double speed, double t_recv)
{
    sync_peer p;
    memset(&p, 0, sizeof(p));
    snprintf(p.id, sizeof(p.id), "%s", id);
    p.state = st;
    p.has_pos = has_pos;
    p.pos0 = pos;
    p.speed = speed;
    p.t_recv = t_recv;
    return p;
}

static int count_kind(const sync_cmd *cmds, int n, sync_cmd_kind k)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        if (cmds[i].kind == k)
            c++;
    return c;
}

static double kind_value(const sync_cmd *cmds, int n, sync_cmd_kind k)
{
    for (int i = 0; i < n; i++)
        if (cmds[i].kind == k)
            return cmds[i].value;
    return -999;
}

static void test_eligibility(void)
{
    sync_peer p = mkpeer("a", ST_PLAYING, 1, 10, 1.0, 100.0);
    CHECK(sync_peer_fresh(&p, 100.0) == 1);
    CHECK(sync_peer_fresh(&p, 105.0) == 1);
    CHECK(sync_peer_fresh(&p, 105.001) == 0); /* stale after 5 s */
    p.joining = 1;
    CHECK(sync_peer_fresh(&p, 100.0) == 0);
    p.joining = 0;
    p.observer = 1;
    CHECK(sync_peer_fresh(&p, 100.0) == 0);
    p.observer = 0;
    p.has_pos = 0;
    CHECK(sync_peer_fresh(&p, 100.0) == 0);
}

static void test_group_min(void)
{
    double now = 1000.0;
    sync_peer peers[4];
    peers[0] = mkpeer("a", ST_PLAYING, 1, 10.0, 1.0, now);
    peers[1] = mkpeer("b", ST_PLAYING, 1, 20.0, 1.0, now);
    peers[2] = mkpeer("c", ST_PAUSED, 1, 5.0, 0.0, now);
    peers[3] = mkpeer("d", ST_PLAYING, 1, 30.0, 1.0, now);
    CHECK_NEAR(sync_group_min(peers, 4, now), 10.0, 1e-9);

    /* Extrapolation advances each candidate by local elapsed time. */
    CHECK_NEAR(sync_group_min(peers, 4, now + 2.0), 12.0, 1e-9);

    /* Paused peers do not count toward the minimum. */
    peers[0].state = ST_PAUSED;
    peers[1].state = ST_PAUSED;
    peers[3].state = ST_PAUSED;
    CHECK_NEAR(sync_group_min(peers, 4, now), -1.0, 1e-9);

    /* Joining and observer peers are excluded. */
    peers[0].state = ST_PLAYING;
    peers[0].joining = 1;
    CHECK_NEAR(sync_group_min(peers, 4, now), -1.0, 1e-9);

    /* Stale peers are excluded. */
    peers[0].joining = 0;
    CHECK_NEAR(sync_group_min(peers, 4, now + 6.0), -1.0, 1e-9);
}

static void test_group_static(void)
{
    double now = 50.0;
    sync_peer peers[3];
    peers[0] = mkpeer("a", ST_PAUSED, 1, 40.0, 0.0, now);
    peers[1] = mkpeer("b", ST_EOF, 1, 30.0, 0.0, now);
    peers[2] = mkpeer("c", ST_PLAYING, 1, 10.0, 1.0, now);
    CHECK_NEAR(sync_group_static(peers, 3, now), 30.0, 1e-9);
    peers[1].joining = 1;
    CHECK_NEAR(sync_group_static(peers, 3, now), 40.0, 1e-9);
    peers[0].state = ST_PLAYING;
    peers[1].joining = 0;
    peers[1].state = ST_PLAYING;
    CHECK_NEAR(sync_group_static(peers, 3, now), -1.0, 1e-9);
}

static void test_group_state(void)
{
    double now = 10.0;
    sync_peer peers[2];
    peers[0] = mkpeer("a", ST_PAUSED, 1, 1.0, 0.0, now);
    peers[1] = mkpeer("b", ST_PLAYING, 1, 2.0, 1.0, now);
    CHECK(sync_group_state(peers, 2, now) == SYNC_GS_PLAYING);
    peers[1].state = ST_PAUSED;
    CHECK(sync_group_state(peers, 2, now) == SYNC_GS_PAUSED);
    peers[0].state = ST_IDLE;
    CHECK(sync_group_state(peers, 2, now) == SYNC_GS_PAUSED);
    peers[1].state = ST_BUFFERING;
    CHECK(sync_group_state(peers, 2, now) == SYNC_GS_UNKNOWN);
}

static void test_deadband_and_nudge(void)
{
    CHECK_NEAR(sync_deadband(0.0), 0.150, 1e-9);
    CHECK_NEAR(sync_deadband(0.050), 0.150, 1e-9);
    CHECK_NEAR(sync_deadband(0.200), 0.250, 1e-9);
    CHECK_NEAR(sync_deadband(2.0), 1.000, 1e-9);

    CHECK_NEAR(sync_nudge_speed(0.0), 0.99, 1e-9);      /* clamp min 1 % */
    CHECK_NEAR(sync_nudge_speed(0.1), 0.99, 1e-9);
    CHECK_NEAR(sync_nudge_speed(0.4), 0.96, 1e-9);
    CHECK_NEAR(sync_nudge_speed(1.0), 0.95, 1e-9);      /* clamp max 5 % */
    CHECK_NEAR(sync_nudge_speed(9.0), 0.95, 1e-9);
}

static sync_input playing_input(sync_peer *peers, int n, double now, double pos)
{
    sync_input in;
    memset(&in, 0, sizeof(in));
    in.now = now;
    in.rtt_self = 0.0; /* deadband 150 ms */
    in.state = ST_PLAYING;
    in.has_pos = 1;
    in.pos = pos;
    in.peers = peers;
    in.npeers = n;
    return in;
}

static void test_tick_no_correction_when_alone(void)
{
    sync_state ss;
    sync_state_init(&ss);
    ss.joining = 0;
    ss.was_joining = 0;
    sync_input in = playing_input(NULL, 0, 100.0, 500.0);
    sync_cmd cmds[SYNC_MAX_CMDS];
    int n = sync_tick(&ss, &in, cmds, SYNC_MAX_CMDS);
    CHECK(n == 0);
    CHECK(ss.joining == 0);
}

static void test_tick_ahead_inside_deadband(void)
{
    double now = 100.0;
    sync_peer p = mkpeer("b", ST_PLAYING, 1, 500.0, 1.0, now);
    sync_state ss;
    sync_state_init(&ss);
    ss.joining = 0;
    ss.was_joining = 0;
    sync_input in = playing_input(&p, 1, now, 500.05);
    sync_cmd cmds[SYNC_MAX_CMDS];
    int n = sync_tick(&ss, &in, cmds, SYNC_MAX_CMDS);
    CHECK(n == 0);
}

static void test_tick_nudge_and_hysteresis(void)
{
    double now = 100.0;
    sync_peer p = mkpeer("b", ST_PLAYING, 1, 100.0, 1.0, now);
    sync_state ss;
    sync_state_init(&ss);
    ss.joining = 0;
    ss.was_joining = 0;

    sync_cmd cmds[SYNC_MAX_CMDS];
    sync_input in = playing_input(&p, 1, now, 100.4); /* 0.4 ahead */
    int n = sync_tick(&ss, &in, cmds, SYNC_MAX_CMDS);
    CHECK(count_kind(cmds, n, SYNC_CMD_SPEED) == 1);
    CHECK_NEAR(kind_value(cmds, n, SYNC_CMD_SPEED), 0.96, 1e-9);
    CHECK(count_kind(cmds, n, SYNC_CMD_SEEK) == 0);
    CHECK(ss.nudging == 1);

    /* Re-entry inside 5 s is rate-limited: same ahead, no new speed command. */
    n = sync_tick(&ss, &in, cmds, SYNC_MAX_CMDS);
    CHECK(count_kind(cmds, n, SYNC_CMD_SPEED) == 0);

    /* Converged below deadband/2: exit nudge and restore 1.0. */
    sync_input in2 = playing_input(&p, 1, now + 1.0, 100.05);
    n = sync_tick(&ss, &in2, cmds, SYNC_MAX_CMDS);
    CHECK(count_kind(cmds, n, SYNC_CMD_SPEED) == 1);
    CHECK_NEAR(kind_value(cmds, n, SYNC_CMD_SPEED), 1.0, 1e-9);
    CHECK(ss.nudging == 0);
}

static void test_tick_hard_seek_rate_limited(void)
{
    double now = 100.0;
    sync_peer p = mkpeer("b", ST_PLAYING, 1, 100.0, 1.0, now);
    sync_state ss;
    sync_state_init(&ss);
    ss.joining = 0;
    ss.was_joining = 0;

    sync_cmd cmds[SYNC_MAX_CMDS];
    sync_input in = playing_input(&p, 1, now, 101.5); /* 1.5 ahead */
    int n = sync_tick(&ss, &in, cmds, SYNC_MAX_CMDS);
    CHECK(count_kind(cmds, n, SYNC_CMD_SEEK) == 1);
    CHECK_NEAR(kind_value(cmds, n, SYNC_CMD_SEEK), 100.0, 1e-9);

    /* Within 10 s the hard seek is suppressed (a nudge may still occur). */
    p.t_recv = now + 2.0;
    sync_input in2 = playing_input(&p, 1, now + 2.0, 103.5);
    n = sync_tick(&ss, &in2, cmds, SYNC_MAX_CMDS);
    CHECK(count_kind(cmds, n, SYNC_CMD_SEEK) == 0);

    /* After 10 s another hard seek is permitted. */
    p.t_recv = now + 10.0;
    sync_input in3 = playing_input(&p, 1, now + 10.0, 111.5);
    n = sync_tick(&ss, &in3, cmds, SYNC_MAX_CMDS);
    CHECK(count_kind(cmds, n, SYNC_CMD_SEEK) == 1);
}

static void test_tick_nonplaying_restores_speed(void)
{
    double now = 100.0;
    sync_peer p = mkpeer("b", ST_PLAYING, 1, 100.0, 1.0, now);
    sync_state ss;
    sync_state_init(&ss);
    ss.joining = 0;
    ss.was_joining = 0;
    ss.nudging = 1;
    ss.speed_applied = 0.95;

    sync_cmd cmds[SYNC_MAX_CMDS];
    sync_input in = playing_input(&p, 1, now, 100.05);
    in.state = ST_PAUSED;
    int n = sync_tick(&ss, &in, cmds, SYNC_MAX_CMDS);
    CHECK(count_kind(cmds, n, SYNC_CMD_SPEED) == 1);
    CHECK_NEAR(kind_value(cmds, n, SYNC_CMD_SPEED), 1.0, 1e-9);
    CHECK(count_kind(cmds, n, SYNC_CMD_SEEK) == 0);
}

static void test_join_converges_playing(void)
{
    double now = 100.0;
    sync_peer p = mkpeer("b", ST_PLAYING, 1, 130.0, 1.0, now);
    sync_state ss;
    sync_state_init(&ss);

    sync_cmd cmds[SYNC_MAX_CMDS];
    sync_input in = playing_input(&p, 1, now, 0.0); /* joiner at 0 */
    int n = sync_tick(&ss, &in, cmds, SYNC_MAX_CMDS);
    CHECK(count_kind(cmds, n, SYNC_CMD_SEEK) == 1);
    CHECK_NEAR(kind_value(cmds, n, SYNC_CMD_SEEK), 130.0, 1e-9);
    CHECK(count_kind(cmds, n, SYNC_CMD_PAUSE) == 1);
    CHECK_NEAR(kind_value(cmds, n, SYNC_CMD_PAUSE), 0.0, 1e-9);
    CHECK(ss.joining == 1);

    /* Now within the deadband; needs one unbroken second. */
    p.pos0 = 130.1;
    p.t_recv = now + 0.5;
    sync_input in2 = playing_input(&p, 1, now + 0.5, 130.1);
    n = sync_tick(&ss, &in2, cmds, SYNC_MAX_CMDS);
    CHECK(ss.joining == 1);
    CHECK(count_kind(cmds, n, SYNC_CMD_SEEK) == 0);

    p.pos0 = 130.6;
    p.t_recv = now + 1.6;
    sync_input in3 = playing_input(&p, 1, now + 1.6, 130.6);
    n = sync_tick(&ss, &in3, cmds, SYNC_MAX_CMDS);
    CHECK(ss.joining == 0);
    CHECK(count_kind(cmds, n, SYNC_CMD_PAUSE) == 1);
}

static void test_join_converges_paused(void)
{
    double now = 100.0;
    sync_peer p = mkpeer("b", ST_PAUSED, 1, 42.0, 0.0, now);
    sync_state ss;
    sync_state_init(&ss);

    sync_cmd cmds[SYNC_MAX_CMDS];
    sync_input in = playing_input(&p, 1, now, 0.0);
    int n = sync_tick(&ss, &in, cmds, SYNC_MAX_CMDS);
    CHECK(count_kind(cmds, n, SYNC_CMD_SEEK) == 1);
    CHECK_NEAR(kind_value(cmds, n, SYNC_CMD_SEEK), 42.0, 1e-9);
    CHECK(count_kind(cmds, n, SYNC_CMD_PAUSE) == 1);
    CHECK_NEAR(kind_value(cmds, n, SYNC_CMD_PAUSE), 1.0, 1e-9);

    p.t_recv = now + 1.1;
    sync_input in2 = playing_input(&p, 1, now + 1.1, 42.0);
    in2.state = ST_PAUSED;
    n = sync_tick(&ss, &in2, cmds, SYNC_MAX_CMDS);
    CHECK(ss.joining == 0);
}

static void test_join_clears_when_unknown(void)
{
    double now = 100.0;
    sync_state ss;
    sync_state_init(&ss);
    sync_cmd cmds[SYNC_MAX_CMDS];
    sync_input in = playing_input(NULL, 0, now, 12.0);
    int n = sync_tick(&ss, &in, cmds, SYNC_MAX_CMDS);
    (void)n;
    CHECK(ss.joining == 0);
}

/* AC-9: a pinned straggler drags the group back, rate-limited to one seek/10 s. */
static void test_pinned_straggler(void)
{
    double now = 1000.0;
    sync_state ss;
    sync_state_init(&ss);
    ss.joining = 0;
    ss.was_joining = 0;

    int seeks = 0;
    double last_seek = -1e9;
    double max_gap = 0;
    double pos = 200.0;
    for (int step = 0; step <= 300; step++) {
        double t = now + step * 0.25; /* 4 Hz for 75 s */
        sync_peer p = mkpeer("pinned", ST_PLAYING, 1, 100.0, 0.0, t);
        pos += 0.25; /* local advances at 1x */
        sync_input in = playing_input(&p, 1, t, pos);
        sync_cmd cmds[SYNC_MAX_CMDS];
        int n = sync_tick(&ss, &in, cmds, SYNC_MAX_CMDS);
        if (count_kind(cmds, n, SYNC_CMD_SEEK)) {
            seeks++;
            if (last_seek > 0) {
                double gap = t - last_seek;
                if (gap > max_gap)
                    max_gap = gap;
            }
            last_seek = t;
            pos = 100.0; /* hard seek lands on the straggler */
        }
    }
    CHECK(seeks >= 7);
    CHECK(max_gap <= 10.5);
}

int main(void)
{
    test_eligibility();
    test_group_min();
    test_group_static();
    test_group_state();
    test_deadband_and_nudge();
    test_tick_no_correction_when_alone();
    test_tick_ahead_inside_deadband();
    test_tick_nudge_and_hysteresis();
    test_tick_hard_seek_rate_limited();
    test_tick_nonplaying_restores_speed();
    test_join_converges_playing();
    test_join_converges_paused();
    test_join_clears_when_unknown();
    test_pinned_straggler();

    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok", checks, fails);
    return fails ? 1 : 0;
}
