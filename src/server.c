#include "server.h"
#include "net.h"
#include "proto.h"
#include "timebase.h"

#include "cJSON.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_MAX_CONN (PS_MAX_MEMBERS_CEIL + 32)

typedef struct sconn {
    int fd;
    int closing;
    double closing_since;
    int ready;
    char id[PS_ID_LEN];
    char name[PS_NAME_LEN];
    int observer;
    double last_traffic;
    lreader lr;
    obuf out;
} sconn;

typedef struct {
    const server_config *cfg;
    int listen_fd;
    char session[PS_ID_LEN];
    sconn *conns[SERVER_MAX_CONN];
    int nconns;
    unsigned long relayed;
    unsigned long accepted;
    double last_stats;
} server;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Stop reading from c and close it once its output drains (or the drain
 * deadline passes). */
static void mark_closing(sconn *c)
{
    if (c->closing)
        return;
    c->closing = 1;
    c->closing_since = tb_now();
}

/*
 * Queue a framed message for c. When the queue cannot take it, c is closed:
 * a slow consumer over its backlog limit has its backlog discarded so it is
 * dropped promptly and its memory is released at once.
 */
static void conn_queue(sconn *c, const char *msg, size_t len, const char *what)
{
    int r = obuf_append_line(&c->out, msg, len);
    if (r == 0)
        return;
    if (r == -2) {
        log_warn("server: output backlog limit reached for member '%s' "
                 "(%zu bytes queued, %zu buffered in total); dropping it",
                 c->ready ? c->id : "?", obuf_pending_bytes(&c->out), obuf_total_allocated());
        obuf_free(&c->out);
    } else {
        log_warn("server: out of memory queueing %s; closing connection", what);
    }
    mark_closing(c);
}

static void conn_send(sconn *c, char *msg)
{
    if (msg == NULL)
        return;
    conn_queue(c, msg, strlen(msg), "a message");
    free(msg);
}

static int ready_count(server *s)
{
    int n = 0;
    for (int i = 0; i < s->nconns; i++)
        if (s->conns[i]->ready)
            n++;
    return n;
}

/* Snapshot the roster from the ready connections; returns the member count. */
static int fill_members(const server *s, proto_member *members)
{
    int n = 0;
    for (int i = 0; i < s->nconns && n < PS_MAX_MEMBERS_CEIL; i++) {
        const sconn *c = s->conns[i];
        if (!c->ready)
            continue;
        snprintf(members[n].id, sizeof(members[n].id), "%s", c->id);
        snprintf(members[n].name, sizeof(members[n].name), "%s", c->name);
        members[n].observer = c->observer;
        n++;
    }
    return n;
}

static void broadcast_roster(server *s)
{
    proto_member members[PS_MAX_MEMBERS_CEIL];
    int n = fill_members(s, members);
    char *msg = proto_encode_roster(members, n);
    if (!msg)
        return;
    for (int i = 0; i < s->nconns; i++) {
        sconn *c = s->conns[i];
        if (!c->ready || c->closing)
            continue;
        conn_queue(c, msg, strlen(msg), "roster");
    }
    free(msg);
}

static void relay_except(server *s, sconn *from, const char *msg)
{
    size_t len = strlen(msg);
    for (int i = 0; i < s->nconns; i++) {
        sconn *c = s->conns[i];
        if (!c->ready || c == from || c->closing)
            continue;
        conn_queue(c, msg, len, "a relayed message");
    }
}

static void close_conn(server *s, sconn *c)
{
    int was_ready = c->ready;
    if (was_ready)
        log_info("server: member '%s' (%s) disconnected", c->name[0] ? c->name : "?", c->id);
    int idx = -1;
    for (int i = 0; i < s->nconns; i++) {
        if (s->conns[i] == c) {
            idx = i;
            break;
        }
    }
    if (idx >= 0) {
        for (int i = idx; i + 1 < s->nconns; i++)
            s->conns[i] = s->conns[i + 1];
        s->nconns--;
    }
    net_close(c->fd);
    lr_free(&c->lr);
    obuf_free(&c->out);
    free(c);
    if (was_ready)
        broadcast_roster(s);
}

static void reject(sconn *c, err_code code, const char *emsg)
{
    char *msg = proto_encode_error(code, emsg, 1);
    conn_send(c, msg);
    c->ready = 0;
    mark_closing(c);
    log_info("server: rejected connection: %s (%s)", err_name(code), emsg ? emsg : "");
}

static void handle_hello(server *s, sconn *c, const pmsg *m)
{
    if (m->v != PS_PROTO_VERSION) {
        reject(c, ERR_UNSUPPORTED_VERSION, "protocol version not supported");
        return;
    }
    if (m->id[0] == '\0') {
        reject(c, ERR_BAD_JSON, "hello missing id");
        return;
    }
    for (int i = 0; i < s->nconns; i++) {
        sconn *o = s->conns[i];
        if (o != c && o->ready && strcmp(o->id, m->id) == 0) {
            reject(c, ERR_DUPLICATE_ID, "id already in session");
            return;
        }
    }
    if (ready_count(s) >= s->cfg->max_members) {
        reject(c, ERR_SESSION_FULL, "session is full");
        return;
    }
    snprintf(c->id, sizeof(c->id), "%s", m->id);
    snprintf(c->name, sizeof(c->name), "%s", m->name);
    c->observer = m->observer;
    c->ready = 1;
    c->last_traffic = tb_now();
    s->accepted++;

    proto_member members[PS_MAX_MEMBERS_CEIL];
    int n = fill_members(s, members);
    char *w = proto_encode_welcome(s->session, c->id, members, n);
    conn_send(c, w);
    log_info("server: member '%s' (%s) joined%s [%d/%d]", c->name, c->id,
             c->observer ? " observer" : "", ready_count(s), s->cfg->max_members);
    broadcast_roster(s);
}

static void reject_bad_json(sconn *c)
{
    conn_send(c, proto_encode_error(ERR_BAD_JSON, "not a JSON object", 0));
}

/* Parse a line as a JSON object; on failure reject the bad JSON and return NULL. */
static cJSON *parse_line_object(sconn *c, const char *line, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        reject_bad_json(c);
        return NULL;
    }
    return root;
}

/* Blind relay for hb/intent: only "from" is touched. */
static void handle_relay(server *s, sconn *c, const char *line, size_t len)
{
    cJSON *root = parse_line_object(c, line, len);
    if (root == NULL)
        return;
    cJSON_DeleteItemFromObjectCaseSensitive(root, "from");
    cJSON_AddStringToObject(root, "from", c->id);
    char *msg = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (msg) {
        relay_except(s, c, msg);
        s->relayed++;
        free(msg);
    }
}

static void handle_line(server *s, sconn *c, const char *line, size_t len)
{
    if (len == 0)
        return;
    c->last_traffic = tb_now();

    if (!c->ready) {
        pmsg m;
        int rc = proto_parse(line, len, &m);
        if (rc == -1) {
            reject(c, ERR_BAD_JSON, "hello is not valid JSON");
            return;
        }
        if (rc == -2 || m.t != MSG_HELLO) {
            reject(c, ERR_UNKNOWN_TYPE, "first message is not hello");
            return;
        }
        handle_hello(s, c, &m);
        return;
    }

    cJSON *root = parse_line_object(c, line, len);
    if (root == NULL)
        return;
    const cJSON *titem = cJSON_GetObjectItemCaseSensitive(root, "t");
    const char *t = (cJSON_IsString(titem) && titem->valuestring) ? titem->valuestring : "";
    int is_hb = strcmp(t, "hb") == 0;
    int is_intent = strcmp(t, "intent") == 0;
    int is_ping = strcmp(t, "ping") == 0;
    int is_bye = strcmp(t, "bye") == 0;
    int is_error = strcmp(t, "error") == 0;
    char bye_reason[PS_REASON_LEN] = "";
    if (is_bye) {
        const cJSON *ritem = cJSON_GetObjectItemCaseSensitive(root, "reason");
        if (cJSON_IsString(ritem) && ritem->valuestring)
            snprintf(bye_reason, sizeof(bye_reason), "%s", ritem->valuestring);
    }
    cJSON_Delete(root);

    if (is_hb || is_intent) {
        handle_relay(s, c, line, len);
    } else if (is_ping) {
        pmsg m;
        proto_parse(line, len, &m);
        conn_send(c, proto_encode_pong(m.n));
    } else if (is_bye) {
        log_info("server: member '%s' sent bye (%s)", c->id,
                 bye_reason[0] ? bye_reason : "unspecified");
        c->ready = 0;
        mark_closing(c);
    } else if (is_error) {
        log_warn("server: member '%s' reported an error", c->id);
    } else {
        conn_send(c, proto_encode_error(ERR_UNKNOWN_TYPE, "unknown message type", 0));
    }
}

static void process_input(server *s, sconn *c)
{
    for (;;) {
        ssize_t r = lr_fill(c->fd, &c->lr);
        if (r > 0) {
            char *line;
            size_t len;
            int lr;
            while ((lr = lr_next(&c->lr, &line, &len)) == 1) {
                handle_line(s, c, line, len);
                if (c->closing)
                    return;
            }
            if (lr < 0) {
                conn_send(c, proto_encode_error(ERR_TOO_LARGE, "message exceeds 64 KiB", 1));
                mark_closing(c);
                log_warn("server: member exceeded the 64 KiB message cap");
                return;
            }
        } else if (r == -1) {
            if (c->lr.overflow) {
                conn_send(c, proto_encode_error(ERR_TOO_LARGE, "message exceeds 64 KiB", 1));
                mark_closing(c);
                log_warn("server: member exceeded the 64 KiB message cap");
            } else {
                log_warn("server: read error on connection; closing");
                mark_closing(c);
            }
            return;
        } else {
            return; /* EOF or would-block */
        }
    }
}

int server_run(const server_config *cfg)
{
    server s;
    memset(&s, 0, sizeof(s));
    s.cfg = cfg;
    s.listen_fd = -1;
    if (ps_uuid4(s.session, sizeof(s.session)) != 0) {
        log_error("server: cannot generate a session id");
        return 1;
    }
    s.last_stats = tb_now();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    s.listen_fd = net_listen_tcp(cfg->bind_addr, cfg->port);
    if (s.listen_fd < 0) {
        log_error("cannot listen on %s:%d: %s", cfg->bind_addr, cfg->port, strerror(errno));
        return 1;
    }
    log_info("server '%s' listening on %s:%d (max %d members, session %s)",
             cfg->name, cfg->bind_addr, cfg->port, cfg->max_members, s.session);

    while (!g_stop) {
        struct pollfd pfds[SERVER_MAX_CONN + 1];
        sconn *map[SERVER_MAX_CONN + 1];
        int n = 0;
        pfds[n].fd = s.listen_fd;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        map[n] = NULL;
        n++;
        for (int i = 0; i < s.nconns; i++) {
            sconn *c = s.conns[i];
            pfds[n].fd = c->fd;
            pfds[n].events = POLLIN | (obuf_pending(&c->out) ? POLLOUT : 0);
            pfds[n].revents = 0;
            map[n] = c;
            n++;
        }

        int pr = poll(pfds, n, 500);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            log_error("poll failed: %s", strerror(errno));
            break;
        }
        double now = tb_now();

        if (pfds[0].revents & POLLIN) {
            for (;;) {
                int fd = accept(s.listen_fd, NULL, NULL);
                if (fd < 0)
                    break;
                net_set_nonblocking(fd);
                net_set_nodelay(fd);
                if (s.nconns >= SERVER_MAX_CONN) {
                    net_close(fd);
                    continue;
                }
                sconn *c = calloc(1, sizeof(*c));
                if (!c) {
                    net_close(fd);
                    continue;
                }
                c->fd = fd;
                c->last_traffic = now;
                lr_init(&c->lr);
                obuf_init(&c->out);
                s.conns[s.nconns++] = c;
                log_trace("server: connection accepted (fd %d)", fd);
            }
        }

        for (int i = 1; i < n; i++) {
            sconn *c = map[i];
            if (c == NULL)
                continue;
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close_conn(&s, c);
                continue;
            }
            if (pfds[i].revents & POLLIN) {
                process_input(&s, c);
                int still = 0;
                for (int j = 0; j < s.nconns; j++)
                    if (s.conns[j] == c) { still = 1; break; }
                if (!still)
                    continue;
            }
            if (obuf_pending(&c->out)) {
                if (obuf_flush(c->fd, &c->out) < 0) {
                    close_conn(&s, c);
                    continue;
                }
            }
            if (c->closing && !obuf_pending(&c->out)) {
                close_conn(&s, c);
                continue;
            }
            if (c->closing && (now - c->closing_since) > PS_DRAIN_TIMEOUT) {
                log_info("server: dropping member '%s' whose output did not drain",
                         c->id[0] ? c->id : "?");
                close_conn(&s, c);
                continue;
            }
            if (!c->closing && (now - c->last_traffic) > PS_LIVENESS_TIMEOUT) {
                log_info("server: reaping idle member '%s'", c->ready ? c->id : "?");
                close_conn(&s, c);
                continue;
            }
        }

        if (g_log_level >= 2 && now - s.last_stats >= 10.0) {
            log_trace("server: %d members, %lu relayed, %lu accepted", ready_count(&s),
                      s.relayed, s.accepted);
            s.last_stats = now;
        }

        if (cfg->exit_when_empty && ready_count(&s) == 0 && s.nconns == 0 && s.accepted > 0) {
            log_info("server: last member left, exiting (--exit-when-empty)");
            break;
        }
    }

    log_info("server: shutting down (%lu relayed, %lu accepted)", s.relayed, s.accepted);
    while (s.nconns > 0)
        close_conn(&s, s.conns[0]);
    if (s.listen_fd >= 0)
        net_close(s.listen_fd);
    return 0;
}
