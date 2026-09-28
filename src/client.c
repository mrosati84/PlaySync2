#include "client.h"
#include "mpv.h"
#include "net.h"
#include "proto.h"
#include "sync.h"
#include "timebase.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    SS_BACKOFF = 0,
    SS_CONNECTING,
    SS_JOINING,
    SS_CONVERGED
} session_state;

typedef struct {
    char id[PS_ID_LEN];
    char name[PS_NAME_LEN];
    int observer;
    int has_pos;
    double pos0;
    double speed;
    pstate state;
    int joining;
    double t_recv;
} peer_ent;

struct client {
    const client_config *cfg;
    int server_fd;
    lreader lr;
    obuf out;
    session_state ss;
    char self_id[PS_ID_LEN];
    char self_name[PS_NAME_LEN];
    char session[PS_ID_LEN];
    int observer;

    peer_ent peers[PS_MAX_MEMBERS_CEIL];
    int npeers;

    sync_state sync;
    mpv_t *mpv;

    double last_hb;
    double last_ping;
    double last_tick;
    double last_status;
    long ping_n;
    struct {
        long n;
        double sent;
    } pings[16];
    int ping_head, ping_count;

    double rtt_ewma;
    int have_rtt;
    double last_server_rx;

    int backoff_attempt;
    double backoff_until;

    char *host;
    int port;
};

static struct client *g_client = NULL;
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_mpv_pid = 0;

static void on_signal(int sig)
{
    g_stop = 1;
    pid_t pid = (pid_t)g_mpv_pid;
    if (pid > 0)
        kill(pid, sig);
}

static peer_ent *find_peer(struct client *c, const char *id)
{
    for (int i = 0; i < c->npeers; i++)
        if (strcmp(c->peers[i].id, id) == 0)
            return &c->peers[i];
    return NULL;
}

static peer_ent *add_peer(struct client *c, const char *id)
{
    if (c->npeers >= PS_MAX_MEMBERS_CEIL)
        return NULL;
    peer_ent *p = &c->peers[c->npeers++];
    memset(p, 0, sizeof(*p));
    snprintf(p->id, sizeof(p->id), "%s", id);
    p->speed = 1.0;
    p->has_pos = 0;
    p->state = ST_IDLE;
    return p;
}

static void clear_peers(struct client *c)
{
    c->npeers = 0;
}

/* Rebuild the roster: keep heartbeat data by id, refresh name/observer. */
static void apply_roster(struct client *c, const proto_member *members, int n)
{
    peer_ent old[PS_MAX_MEMBERS_CEIL];
    int nold = c->npeers;
    memcpy(old, c->peers, sizeof(old));
    c->npeers = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(members[i].id, c->self_id) == 0)
            continue;
        peer_ent *p = add_peer(c, members[i].id);
        if (!p)
            break;
        snprintf(p->name, sizeof(p->name), "%s", members[i].name);
        p->observer = members[i].observer;
        for (int j = 0; j < nold; j++) {
            if (strcmp(old[j].id, members[i].id) == 0) {
                p->has_pos = old[j].has_pos;
                p->pos0 = old[j].pos0;
                p->speed = old[j].speed;
                p->state = old[j].state;
                p->joining = old[j].joining;
                p->t_recv = old[j].t_recv;
                break;
            }
        }
    }
}

static void build_sync_peers(struct client *c, sync_peer *out, int *nout)
{
    int n = 0;
    for (int i = 0; i < c->npeers && n < PS_MAX_MEMBERS_CEIL; i++) {
        peer_ent *p = &c->peers[i];
        sync_peer *s = &out[n++];
        memset(s, 0, sizeof(*s));
        snprintf(s->id, sizeof(s->id), "%s", p->id);
        s->has_pos = p->has_pos;
        s->pos0 = p->pos0;
        s->speed = p->speed;
        s->state = p->state;
        s->joining = p->joining;
        s->observer = p->observer;
        s->t_recv = p->t_recv;
    }
    *nout = n;
}

static void client_send(struct client *c, char *msg)
{
    if (msg == NULL)
        return;
    if (g_log_level >= 2)
        log_trace("srv <- %s", msg);
    obuf_append(&c->out, msg, strlen(msg));
    obuf_append(&c->out, "\n", 1);
    if (c->server_fd >= 0)
        (void)obuf_flush(c->server_fd, &c->out);
    free(msg);
}

static double group_ref(struct client *c, double now)
{
    sync_peer sp[PS_MAX_MEMBERS_CEIL];
    int n = 0;
    build_sync_peers(c, sp, &n);
    int gs = sync_group_state(sp, n, now);
    if (gs == SYNC_GS_PLAYING)
        return sync_group_min(sp, n, now);
    if (gs == SYNC_GS_PAUSED)
        return sync_group_static(sp, n, now);
    return -1.0;
}

static void send_hb(struct client *c)
{
    int has_pos = 0;
    double pos = 0;
    pstate st = ST_IDLE;
    double speed = 1.0;
    if (c->mpv) {
        has_pos = mpv_has_pos(c->mpv);
        pos = mpv_pos(c->mpv);
        st = mpv_state(c->mpv);
        speed = mpv_engine_speed(c->mpv);
    }
    char *msg = proto_encode_hb(NULL, has_pos, pos, st, speed, c->sync.joining);
    client_send(c, msg);
}

static void send_ping(struct client *c)
{
    c->ping_n++;
    struct client *cc = c;
    long n = c->ping_n;
    cc->pings[cc->ping_head].n = n;
    cc->pings[cc->ping_head].sent = tb_now();
    cc->ping_head = (cc->ping_head + 1) % 16;
    if (cc->ping_count < 16)
        cc->ping_count++;
    char *msg = proto_encode_ping(n);
    client_send(c, msg);
}

static void on_pong(struct client *c, long n, double now)
{
    for (int i = 0; i < 16; i++) {
        if (c->pings[i].n == n && c->pings[i].sent > 0) {
            double rtt = now - c->pings[i].sent;
            if (rtt < 0)
                rtt = 0;
            if (!c->have_rtt) {
                c->rtt_ewma = rtt;
                c->have_rtt = 1;
            } else {
                c->rtt_ewma = 0.25 * rtt + 0.75 * c->rtt_ewma;
            }
            c->pings[i].sent = 0;
            if (c->mpv)
                mpv_set_rtt(c->mpv, c->rtt_ewma);
            return;
        }
    }
}

static void client_hold(struct client *c, double now)
{
    double hold = c->have_rtt ? c->rtt_ewma * 3.0 : 0.5;
    if (hold < 1.0)
        hold = 1.0;
    sync_hold(&c->sync, now, hold);
}

static void broadcast_intent(struct client *c, intent_act act, double pos)
{
    if (c->observer || c->sync.joining)
        return;
    char *msg = proto_encode_intent(NULL, act, pos);
    client_send(c, msg);
}

static void local_pause_intent(struct client *c, int pause)
{
    if (c->observer || c->mpv == NULL || c->sync.joining)
        return;
    if (!mpv_has_pos(c->mpv))
        return;
    double now = tb_now();
    double pos = mpv_pos(c->mpv);
    double gm = group_ref(c, now);
    if (gm >= 0.0 && gm < pos)
        pos = gm;
    mpv_apply_pause(c->mpv, pause, pos);
    client_hold(c, now);
    broadcast_intent(c, pause ? ACT_PAUSE : ACT_RESUME, pos);
}

static void local_seek_intent(struct client *c, double pos)
{
    if (c->observer || c->mpv == NULL || c->sync.joining)
        return;
    client_hold(c, tb_now());
    broadcast_intent(c, ACT_SEEK, pos);
}

static void apply_remote_intent(struct client *c, intent_act act, double pos)
{
    if (c->observer || c->mpv == NULL)
        return;
    if (act == ACT_PAUSE)
        mpv_apply_pause(c->mpv, 1, pos);
    else if (act == ACT_RESUME)
        mpv_apply_pause(c->mpv, 0, pos);
    else if (act == ACT_SEEK)
        mpv_apply_seek(c->mpv, pos);
    client_hold(c, tb_now());
}

static void handle_mpv_events(struct client *c)
{
    if (c->mpv == NULL)
        return;
    mpv_event e;
    while (mpv_next_event(c->mpv, &e)) {
        switch (e.kind) {
        case MPV_EV_PAUSE:
            local_pause_intent(c, e.pause);
            break;
        case MPV_EV_EOF:
            local_pause_intent(c, 1);
            break;
        case MPV_EV_SEEK:
            local_seek_intent(c, e.pos);
            break;
        case MPV_EV_SPEED:
            if (c->cfg->allow_local_speed) {
                log_warn("mpv: local speed change accepted (--allow-local-speed)");
            } else {
                log_info("mpv: reverting local speed change to %.3f", mpv_engine_speed(c->mpv));
                mpv_cmd_speed(c->mpv, mpv_engine_speed(c->mpv));
            }
            break;
        case MPV_EV_LOADED:
            log_trace("mpv: file loaded");
            break;
        case MPV_EV_START:
            log_info("client: new file/playlist entry, re-joining");
            sync_begin_join(&c->sync, tb_now());
            c->ss = SS_JOINING;
            break;
        default:
            break;
        }
    }
}

static void handle_server_msg(struct client *c, const char *line, size_t len)
{
    if (g_log_level >= 2)
        log_trace("srv -> %s", line);
    pmsg m;
    int rc = proto_parse(line, len, &m);
    if (rc == -1) {
        char *e = proto_encode_error(ERR_BAD_JSON, "not valid JSON", 0);
        client_send(c, e);
        return;
    }
    if (rc == -2) {
        char *e = proto_encode_error(ERR_UNKNOWN_TYPE, "unknown message type", 0);
        client_send(c, e);
        return;
    }
    double now = tb_now();
    switch (m.t) {
    case MSG_WELCOME:
        snprintf(c->session, sizeof(c->session), "%s", m.session);
        log_info("client: joined session %s as %s (%d member%s)", m.session, c->self_id,
                 m.nmembers, m.nmembers == 1 ? "" : "s");
        clear_peers(c);
        apply_roster(c, m.members, m.nmembers);
        c->ss = SS_JOINING;
        sync_begin_join(&c->sync, now);
        c->backoff_attempt = 0;
        c->last_server_rx = now;
        break;
    case MSG_ROSTER:
        apply_roster(c, m.members, m.nmembers);
        break;
    case MSG_HB: {
        peer_ent *p = find_peer(c, m.id);
        if (p == NULL)
            p = add_peer(c, m.id);
        if (p) {
            p->has_pos = m.has_pos;
            p->pos0 = m.pos;
            p->speed = m.speed;
            p->state = m.state;
            p->joining = m.joining;
            p->t_recv = now;
        }
        break;
    }
    case MSG_INTENT:
        if (m.act != ACT_NONE)
            apply_remote_intent(c, m.act, m.pos);
        break;
    case MSG_PONG:
        on_pong(c, m.n, now);
        break;
    case MSG_BYE:
        log_info("client: server closed the session (%s)", m.reason);
        break;
    case MSG_ERROR:
        log_warn("client: server error %s: %s%s", err_name(m.code), m.emsg,
                 m.fatal ? " (fatal)" : "");
        break;
    default:
        break;
    }
}

static void handle_server_input(struct client *c)
{
    for (;;) {
        ssize_t r = lr_fill(c->server_fd, &c->lr);
        if (r > 0) {
            c->last_server_rx = tb_now();
            char *line;
            size_t len;
            int lr;
            while ((lr = lr_next(&c->lr, &line, &len)) == 1)
                handle_server_msg(c, line, len);
            if (lr < 0) {
                log_error("client: server message exceeds the 64 KiB cap");
                c->server_fd = -1;
                return;
            }
        } else if (r == 0) {
            log_warn("client: server connection closed");
            c->server_fd = -1;
            return;
        } else if (r == -1) {
            log_warn("client: server connection error");
            c->server_fd = -1;
            return;
        } else {
            return;
        }
    }
}

static void schedule_backoff(struct client *c, const char *why)
{
    if (c->server_fd >= 0) {
        net_close(c->server_fd);
        c->server_fd = -1;
        lr_free(&c->lr);
        lr_init(&c->lr);
        obuf_free(&c->out);
        obuf_init(&c->out);
    }
    int b = c->backoff_attempt;
    if (b < 0)
        b = 0;
    double delay = (b <= 0) ? 1.0 : (b >= 4 ? 15.0 : (double)(1 << b));
    c->backoff_attempt = b + 1;
    c->backoff_until = tb_now() + delay;
    c->ss = SS_BACKOFF;
    if (why)
        log_warn("client: %s; retrying in %.0f s", why, delay);
}

static void try_connect(struct client *c)
{
    log_info("client: connecting to %s:%d", c->host, c->port);
    int fd = net_connect_tcp(c->host, c->port, 1.5);
    if (fd < 0) {
        schedule_backoff(c, "server unreachable");
        return;
    }
    c->server_fd = fd;
    lr_free(&c->lr);
    lr_init(&c->lr);
    obuf_free(&c->out);
    obuf_init(&c->out);
    ps_uuid4(c->self_id, sizeof(c->self_id));
    c->ss = SS_CONNECTING;
    c->last_server_rx = tb_now();
    c->last_ping = 0;
    c->ping_head = c->ping_count = 0;
    c->have_rtt = 0;
    for (int i = 0; i < 16; i++)
        c->pings[i].sent = 0;
    char *hello = proto_encode_hello(c->self_id, c->self_name, c->observer);
    client_send(c, hello);
}

static void do_tick(struct client *c, double now)
{
    if (c->mpv && c->cfg->show_drift) {
        double g = group_ref(c, now);
        if (g >= 0.0 && mpv_has_pos(c->mpv)) {
            char buf[96];
            double drift = mpv_pos(c->mpv) - g;
            snprintf(buf, sizeof(buf), "drift %+.2fs", drift);
            mpv_show_text(c->mpv, buf);
        }
    }
    if (c->mpv == NULL) {
        /* Observers are never eligible and never join a group. */
        c->sync.joining = 0;
        c->sync.was_joining = 0;
        return;
    }

    sync_peer sp[PS_MAX_MEMBERS_CEIL];
    int n = 0;
    build_sync_peers(c, sp, &n);

    sync_input in;
    in.now = now;
    in.rtt_self = c->have_rtt ? c->rtt_ewma : 0.05;
    in.state = mpv_state(c->mpv);
    in.has_pos = mpv_has_pos(c->mpv);
    in.pos = mpv_pos(c->mpv);
    in.observer = c->observer;
    in.peers = sp;
    in.npeers = n;

    sync_cmd cmds[SYNC_MAX_CMDS];
    int nc = sync_tick(&c->sync, &in, cmds, SYNC_MAX_CMDS);
    for (int i = 0; i < nc; i++) {
        switch (cmds[i].kind) {
        case SYNC_CMD_SEEK:
            mpv_cmd_seek(c->mpv, cmds[i].value);
            break;
        case SYNC_CMD_SPEED:
            mpv_cmd_speed(c->mpv, cmds[i].value);
            break;
        case SYNC_CMD_PAUSE:
            mpv_cmd_pause(c->mpv, cmds[i].value >= 0.5);
            break;
        default:
            break;
        }
    }
    if (c->ss == SS_JOINING && !c->sync.joining) {
        c->ss = SS_CONVERGED;
        log_info("client: converged with the group");
    }
}

static void show_status(struct client *c, double now)
{
    if (g_log_level < 1) /* only --quiet suppresses the live status (FR-6.1) */
        return;
    double g = group_ref(c, now);
    double pos = c->mpv ? mpv_pos(c->mpv) : 0.0;
    int has_pos = c->mpv && mpv_has_pos(c->mpv);
    char groupbuf[32];
    if (g >= 0.0)
        snprintf(groupbuf, sizeof(groupbuf), "%.2f", g);
    else
        snprintf(groupbuf, sizeof(groupbuf), "-");
    char aheadbuf[32];
    if (g >= 0.0 && has_pos)
        snprintf(aheadbuf, sizeof(aheadbuf), "%+.3f", pos - g);
    else
        snprintf(aheadbuf, sizeof(aheadbuf), "-");
    char posbuf[32];
    if (has_pos)
        snprintf(posbuf, sizeof(posbuf), "%.2f", pos);
    else
        snprintf(posbuf, sizeof(posbuf), "-");
    const char *ss = c->ss == SS_CONVERGED ? "converged" :
                     c->ss == SS_JOINING ? "joining" :
                     c->ss == SS_CONNECTING ? "connecting" : "partitioned";
    char line[256];
    snprintf(line, sizeof(line),
             "state=%s session=%s pos=%s group=%s ahead=%s rtt=%.0fms members=%d",
             c->mpv ? pstate_name(mpv_state(c->mpv)) : "idle", ss,
             posbuf, groupbuf, aheadbuf,
             c->have_rtt ? c->rtt_ewma * 1000.0 : 0.0, c->npeers + 1);
    fprintf(stderr, "\r%-100s", line);
    fflush(stderr);
}

int client_run(const client_config *cfg)
{
    struct client c;
    memset(&c, 0, sizeof(c));
    c.cfg = cfg;
    c.host = (char *)cfg->host;
    c.port = cfg->port;
    c.server_fd = -1;
    c.observer = cfg->no_mpv;
    sync_state_init(&c.sync);
    snprintf(c.self_name, sizeof(c.self_name), "%s", cfg->name);
    snprintf(c.self_id, sizeof(c.self_id), "self");
    lr_init(&c.lr);
    obuf_init(&c.out);
    c.backoff_attempt = 0;
    c.backoff_until = 0;
    for (int i = 0; i < 16; i++)
        c.pings[i].sent = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    g_client = &c;

    char sockpath[256] = "";
    if (!cfg->no_mpv) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg && *xdg)
            snprintf(sockpath, sizeof(sockpath), "%s/playsync2-%ld.sock", xdg, (long)getpid());
        else
            snprintf(sockpath, sizeof(sockpath), "/tmp/playsync2-%ld.sock", (long)getpid());
        (void)unlink(sockpath);
        char err[256] = "";
        c.mpv = mpv_spawn(sockpath, cfg->mpv_args, cfg->mpv_argc, g_log_level >= 2, err,
                          sizeof(err));
        if (c.mpv == NULL) {
            log_error("mpv: %s", err);
            lr_free(&c.lr);
            obuf_free(&c.out);
            g_client = NULL;
            return 1;
        }
        g_mpv_pid = (sig_atomic_t)mpv_pid(c.mpv);
        mpv_set_allow_local_speed(c.mpv, cfg->allow_local_speed);
    }

    int exit_code = 0;
    int mpv_exit = 0;
    double now = tb_now();
    c.last_hb = now;
    c.last_ping = now;
    c.last_tick = now;
    c.last_status = now;

    while (!g_stop) {
        now = tb_now();

        if (c.server_fd < 0 && now >= c.backoff_until)
            try_connect(&c);

        if (c.mpv) {
            int status = 0;
            if (mpv_child_status(c.mpv, &status)) {
                if (WIFEXITED(status))
                    exit_code = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    exit_code = 128 + WTERMSIG(status);
                log_info("client: mpv exited (status %d)", exit_code);
                mpv_exit = 1;
                break;
            }
        }

        struct pollfd pfds[2];
        int map[2];
        int n = 0;
        if (c.server_fd >= 0) {
            pfds[n].fd = c.server_fd;
            pfds[n].events = POLLIN | (obuf_pending(&c.out) ? POLLOUT : 0);
            pfds[n].revents = 0;
            map[n] = 1;
            n++;
        }
        if (c.mpv) {
            pfds[n].fd = mpv_fd(c.mpv);
            pfds[n].events = POLLIN | (mpv_pending(c.mpv) ? POLLOUT : 0);
            pfds[n].revents = 0;
            map[n] = 2;
            n++;
        }
        int pr = poll(pfds, n, 100);
        if (pr < 0 && errno != EINTR) {
            log_error("client: poll failed: %s", strerror(errno));
            exit_code = 1;
            break;
        }
        now = tb_now();

        for (int i = 0; i < n; i++) {
            if (map[i] == 1) {
                if (pfds[i].revents & (POLLERR | POLLHUP)) {
                    if (pfds[i].revents & POLLIN)
                        handle_server_input(&c);
                    if (c.server_fd >= 0)
                        schedule_backoff(&c, "server connection lost");
                } else if (pfds[i].revents & POLLIN) {
                    handle_server_input(&c);
                }
                if (c.server_fd >= 0 && (pfds[i].revents & POLLOUT))
                    (void)obuf_flush(c.server_fd, &c.out);
            } else if (map[i] == 2) {
                if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    if (mpv_poll(c.mpv) < 0) {
                        int status = 0;
                        int reaped = 0;
                        /* The socket can EOF a moment before the child is
                         * reapable; give it a brief grace period so an
                         * orderly exit propagates its status (FR-1.5). */
                        for (int w = 0; w < 15; w++) {
                            if (mpv_child_status(c.mpv, &status)) {
                                reaped = 1;
                                break;
                            }
                            struct pollfd z;
                            z.fd = -1;
                            z.events = 0;
                            z.revents = 0;
                            (void)poll(&z, 0, 20);
                        }
                        if (reaped) {
                            if (WIFEXITED(status))
                                exit_code = WEXITSTATUS(status);
                            else if (WIFSIGNALED(status))
                                exit_code = 128 + WTERMSIG(status);
                            log_info("client: mpv exited (status %d)", exit_code);
                            mpv_exit = 1;
                            mpv_destroy(c.mpv, 0);
                            c.mpv = NULL;
                            g_mpv_pid = 0;
                        } else {
                            log_error("client: mpv IPC socket failed; killing mpv");
                            mpv_exit = 1;
                            mpv_destroy(c.mpv, 1);
                            c.mpv = NULL;
                            g_mpv_pid = 0;
                            exit_code = 1;
                        }
                        break;
                    }
                }
                if (mpv_pending(c.mpv) && (pfds[i].revents & POLLOUT))
                    mpv_flush(c.mpv);
            }
        }
        if (c.mpv == NULL && !cfg->no_mpv)
            break;

        handle_mpv_events(&c);

        if (c.server_fd >= 0) {
            if (now - c.last_hb >= PS_HB_INTERVAL) {
                send_hb(&c);
                c.last_hb = now;
            }
            if (now - c.last_ping >= PS_PING_INTERVAL) {
                send_ping(&c);
                c.last_ping = now;
            }
            if (now - c.last_server_rx > PS_PONG_TIMEOUT) {
                schedule_backoff(&c, "no response from server for 15 s");
            }
        }

        if (now - c.last_tick >= 0.25) {
            if (c.mpv)
                mpv_tick(c.mpv);
            do_tick(&c, now);
            c.last_tick = now;
        }
        if (now - c.last_status >= 0.25) {
            show_status(&c, now);
            c.last_status = now;
        }
    }

    if (g_log_level >= 1)
        fprintf(stderr, "\n");
    if (c.server_fd >= 0) {
        char *bye = proto_encode_bye(mpv_exit ? "mpv_exit" : "user_quit");
        if (bye) {
            if (g_log_level >= 2)
                log_trace("srv <- %s", bye);
            obuf_append(&c.out, bye, strlen(bye));
            obuf_append(&c.out, "\n", 1);
            (void)obuf_flush(c.server_fd, &c.out);
            free(bye);
        }
        net_close(c.server_fd);
        c.server_fd = -1;
    }
    if (c.mpv)
        mpv_destroy(c.mpv, 1);
    else if (sockpath[0])
        (void)unlink(sockpath);
    lr_free(&c.lr);
    obuf_free(&c.out);
    g_client = NULL;
    g_mpv_pid = 0;
    return exit_code;
}
