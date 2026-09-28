/*
 * fake_mpv — a tiny MPV JSON-IPC stand-in for headless integration tests.
 *
 * Implements the subset PlaySync2 uses (observe_property, get_property,
 * set_property pause|time-pos|speed, seek, loadfile, show-text, quit) and a
 * simulated timeline. Test hooks, all via the environment:
 *   FAKE_MPV_DURATION     media length in seconds (default 3600)
 *   FAKE_MPV_POS          starting position (default 0)
 *   FAKE_MPV_AUTOPLAY_AT  play automatically after N seconds of runtime
 *   FAKE_MPV_QUIT_AT      exit after N seconds of runtime
 *   FAKE_MPV_LOG          append "t pos state pause speed eof" once a second
 *
 * It starts with a file loaded and paused, matching the client's injected
 * --pause=yes (SPEC §5.1), and accepts exactly one IPC client.
 */
#include "net.h"
#include "timebase.h"

#include "cJSON.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int loaded;
    int pause;
    double pos;
    double speed;
    int eof;
    int seeking;
    int cache;
    int idle;
    int playlist;
    double duration;
} state_t;

static state_t st;
static int observed[16];
static int client_fd = -1;
static obuf out;
static lreader lr;
static FILE *logfile;
static double t0;
static double autoplay_at = -1.0;
static double quit_at = -1.0;
static double seek_at = -1.0;
static double seek_target = 0.0;
static int seek_done;
static double pause_at = -1.0;
static int pause_done;
static int autoplayed;
static double last_log;
static double last_pos_emit;
static double last_timepos_emit;

static void send_raw(const char *s)
{
    if (client_fd < 0)
        return;
    obuf_append(&out, s, strlen(s));
    obuf_append(&out, "\n", 1);
    (void)obuf_flush(client_fd, &out);
}

static void send_obj(cJSON *root)
{
    char *s = root ? cJSON_PrintUnformatted(root) : NULL;
    if (s) {
        send_raw(s);
        free(s);
    }
    cJSON_Delete(root);
}

static void reply(long rid, const char *error)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", error ? error : "success");
    cJSON_AddNumberToObject(o, "request_id", (double)rid);
    send_obj(o);
}

static void emit_bool(int id, const char *name, int v, int have)
{
    if (!observed[id])
        return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "property-change");
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "name", name);
    if (have)
        cJSON_AddBoolToObject(o, "data", v ? 1 : 0);
    send_obj(o);
}

static void emit_num(int id, const char *name, double v, int have)
{
    if (!observed[id])
        return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "property-change");
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "name", name);
    if (have)
        cJSON_AddNumberToObject(o, "data", v);
    send_obj(o);
}

static void emit_for(int id)
{
    switch (id) {
    case 1: emit_bool(1, "pause", st.pause, 1); break;
    case 2: emit_num(2, "time-pos", st.pos, st.loaded); break;
    case 3: emit_bool(3, "seeking", st.seeking, 1); break;
    case 4: emit_bool(4, "eof-reached", st.eof, 1); break;
    case 5: emit_bool(5, "paused-for-cache", st.cache, 1); break;
    case 6: emit_num(6, "speed", st.speed, 1); break;
    case 7: emit_bool(7, "idle-active", st.idle, 1); break;
    case 8: emit_num(8, "playlist-pos", st.playlist, st.playlist >= 0); break;
    default: break;
    }
}

static void emit_seek_sequence(double from)
{
    /* Real MPV emits no property-change when the seek lands on the position it
     * is already at; mimic that so a redundant seek is observable. */
    if (st.pos == from)
        return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "seek");
    send_obj(o);
    emit_num(2, "time-pos", st.pos, 1);
    emit_bool(3, "seeking", 1, 1);
    cJSON *pr = cJSON_CreateObject();
    cJSON_AddStringToObject(pr, "event", "playback-restart");
    send_obj(pr);
    emit_bool(3, "seeking", 0, 1);
}

static double getprop_num(const char *name, int *ok)
{
    *ok = 1;
    if (strcmp(name, "time-pos") == 0)
        return st.pos;
    if (strcmp(name, "speed") == 0)
        return st.speed;
    if (strcmp(name, "playlist-pos") == 0)
        return (double)st.playlist;
    *ok = 0;
    return 0;
}

static void handle_command(const cJSON *root)
{
    const cJSON *riditem = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    long rid = cJSON_IsNumber(riditem) ? (long)riditem->valuedouble : 0;
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsArray(cmd) || cJSON_GetArraySize(cmd) < 1) {
        reply(rid, "invalid parameter");
        return;
    }
    const cJSON *a0 = cJSON_GetArrayItem(cmd, 0);
    const char *op = (cJSON_IsString(a0) && a0->valuestring) ? a0->valuestring : "";

    if (strcmp(op, "observe_property") == 0) {
        const cJSON *idj = cJSON_GetArrayItem(cmd, 1);
        const cJSON *nmj = cJSON_GetArrayItem(cmd, 2);
        int id = cJSON_IsNumber(idj) ? (int)idj->valuedouble : 0;
        const char *nm = (cJSON_IsString(nmj) && nmj->valuestring) ? nmj->valuestring : "";
        if (id > 0 && id < 16) {
            observed[id] = 1;
            reply(rid, NULL);
            emit_for(id);
        } else {
            reply(rid, "invalid parameter");
        }
        (void)nm;
    } else if (strcmp(op, "set_property") == 0) {
        const cJSON *nmj = cJSON_GetArrayItem(cmd, 1);
        const cJSON *vj = cJSON_GetArrayItem(cmd, 2);
        const char *nm = (cJSON_IsString(nmj) && nmj->valuestring) ? nmj->valuestring : "";
        if (strcmp(nm, "pause") == 0 && cJSON_IsBool(vj)) {
            st.pause = cJSON_IsTrue(vj) ? 1 : 0;
            reply(rid, NULL);
            emit_bool(1, "pause", st.pause, 1);
        } else if (strcmp(nm, "speed") == 0 && cJSON_IsNumber(vj)) {
            st.speed = vj->valuedouble;
            reply(rid, NULL);
            emit_num(6, "speed", st.speed, 1);
        } else if (strcmp(nm, "time-pos") == 0 && cJSON_IsNumber(vj)) {
            double from = st.pos;
            st.pos = vj->valuedouble;
            reply(rid, NULL);
            emit_seek_sequence(from);
        } else {
            reply(rid, "property unavailable");
        }
    } else if (strcmp(op, "get_property") == 0) {
        const cJSON *nmj = cJSON_GetArrayItem(cmd, 1);
        const char *nm = (cJSON_IsString(nmj) && nmj->valuestring) ? nmj->valuestring : "";
        int ok = 0;
        double v = getprop_num(nm, &ok);
        if (ok) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "error", "success");
            cJSON_AddNumberToObject(o, "request_id", (double)rid);
            cJSON_AddNumberToObject(o, "data", v);
            send_obj(o);
        } else {
            reply(rid, "property unavailable");
        }
    } else if (strcmp(op, "seek") == 0) {
        const cJSON *vj = cJSON_GetArrayItem(cmd, 1);
        if (cJSON_IsNumber(vj)) {
            double target = vj->valuedouble;
            double from = st.pos;
            /* Quantise to 1/15 s to mimic keyframe landing. */
            double frame = 1.0 / 15.0;
            st.pos = floor(target / frame) * frame;
            st.eof = 0;
            reply(rid, NULL);
            emit_seek_sequence(from);
        } else {
            reply(rid, "invalid parameter");
        }
    } else if (strcmp(op, "loadfile") == 0) {
        st.loaded = 1;
        st.idle = 0;
        st.eof = 0;
        reply(rid, NULL);
        cJSON *sf = cJSON_CreateObject();
        cJSON_AddStringToObject(sf, "event", "start-file");
        send_obj(sf);
        cJSON *fl = cJSON_CreateObject();
        cJSON_AddStringToObject(fl, "event", "file-loaded");
        send_obj(fl);
        emit_bool(7, "idle-active", 0, 1);
        emit_num(8, "playlist-pos", 0, 1);
    } else if (strcmp(op, "show-text") == 0) {
        reply(rid, NULL);
    } else if (strcmp(op, "quit") == 0) {
        reply(rid, NULL);
        if (client_fd >= 0)
            net_close(client_fd);
        exit(0);
    } else {
        reply(rid, "invalid parameter");
    }
}

static void handle_line(const char *line, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (root == NULL)
        return;
    if (cJSON_GetObjectItemCaseSensitive(root, "command") != NULL)
        handle_command(root);
    cJSON_Delete(root);
}

int main(int argc, char **argv)
{
    const char *sockpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--input-ipc-server=", 19) == 0)
            sockpath = argv[i] + 19;
        else if (strcmp(argv[i], "--pause=yes") == 0)
            st.pause = 1;
    }
    if (sockpath == NULL) {
        fprintf(stderr, "fake_mpv: --input-ipc-server=PATH required\n");
        return 2;
    }
    const char *d = getenv("FAKE_MPV_DURATION");
    st.duration = d ? atof(d) : 3600.0;
    const char *p = getenv("FAKE_MPV_POS");
    st.pos = p ? atof(p) : 0.0;
    const char *ap = getenv("FAKE_MPV_AUTOPLAY_AT");
    if (ap)
        autoplay_at = atof(ap);
    const char *qa = getenv("FAKE_MPV_QUIT_AT");
    if (qa)
        quit_at = atof(qa);
    const char *sa = getenv("FAKE_MPV_SEEK_AT");
    if (sa) {
        const char *colon = strchr(sa, ':');
        if (colon) {
            seek_at = atof(sa);
            seek_target = atof(colon + 1);
        }
    }
    const char *pa = getenv("FAKE_MPV_PAUSE_AT");
    if (pa)
        pause_at = atof(pa);
    const char *lf = getenv("FAKE_MPV_LOG");
    if (lf)
        logfile = fopen(lf, "a");
    st.loaded = 1;
    st.idle = 0;
    st.playlist = 0;
    if (!st.pause)
        st.pause = 1; /* default paused like --pause=yes */

    signal(SIGPIPE, SIG_IGN);
    int lfd = net_unix_listen(sockpath);
    if (lfd < 0) {
        fprintf(stderr, "fake_mpv: cannot listen on %s\n", sockpath);
        return 1;
    }
    lr_init(&lr);
    obuf_init(&out);
    t0 = tb_now();
    last_log = t0;
    last_pos_emit = t0;
    last_timepos_emit = t0;

    for (;;) {
        if (client_fd < 0) {
            int fd = accept(lfd, NULL, NULL);
            if (fd >= 0) {
                net_set_nonblocking(fd);
                client_fd = fd;
                observed[0] = 0;
            }
        }
        struct pollfd pfd;
        pfd.fd = client_fd >= 0 ? client_fd : lfd;
        pfd.events = POLLIN | (client_fd >= 0 && obuf_pending(&out) ? POLLOUT : 0);
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 25);
        double now = tb_now();
        if (pr > 0) {
            if (client_fd >= 0) {
                if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
                    for (;;) {
                        ssize_t r = lr_fill(client_fd, &lr);
                        if (r > 0) {
                            char *line;
                            size_t len;
                            while (lr_next(&lr, &line, &len) == 1)
                                handle_line(line, len);
                        } else if (r == 0 || r == -1) {
                            net_close(client_fd);
                            client_fd = -1;
                            break;
                        } else {
                            break;
                        }
                    }
                }
                if (client_fd >= 0 && (pfd.revents & POLLOUT))
                    (void)obuf_flush(client_fd, &out);
            }
        }

        double elapsed = now - t0;
        if (st.loaded && !st.pause && !st.eof && !st.seeking) {
            double dt = now - last_pos_emit;
            if (dt > 0)
                st.pos += dt * st.speed;
            last_pos_emit = now;
            if (st.pos >= st.duration) {
                st.pos = st.duration;
                st.eof = 1;
                st.pause = 1;
                emit_bool(4, "eof-reached", 1, 1);
                emit_bool(1, "pause", 1, 1);
            }
            if (observed[2] && now - last_timepos_emit >= 0.05) {
                emit_num(2, "time-pos", st.pos, 1);
                last_timepos_emit = now;
            }
        } else {
            last_pos_emit = now;
        }

        if (autoplay_at >= 0.0 && !autoplayed && elapsed >= autoplay_at) {
            autoplayed = 1;
            st.pause = 0;
            st.eof = 0;
            emit_bool(1, "pause", 0, 1);
        }
        if (seek_at >= 0.0 && !seek_done && elapsed >= seek_at) {
            seek_done = 1;
            double from = st.pos;
            st.pos = seek_target;
            st.eof = 0;
            emit_seek_sequence(from);
        }
        if (pause_at >= 0.0 && !pause_done && elapsed >= pause_at) {
            pause_done = 1;
            st.pause = 1;
            emit_bool(1, "pause", 1, 1);
        }
        if (logfile && now - last_log >= 1.0) {
            fprintf(logfile, "%.2f pos=%.3f state=%s pause=%d speed=%.3f eof=%d\n",
                    elapsed, st.pos, st.pause ? "paused" : (st.eof ? "eof" : "playing"),
                    st.pause, st.speed, st.eof);
            fflush(logfile);
            last_log = now;
        }
        if (quit_at >= 0.0 && elapsed >= quit_at) {
            net_close(client_fd);
            break;
        }
    }
    int code = 0;
    const char *ec = getenv("FAKE_MPV_EXIT_CODE");
    if (ec)
        code = atoi(ec);
    if (client_fd >= 0)
        net_close(client_fd);
    net_close(lfd);
    unlink(sockpath);
    if (logfile)
        fclose(logfile);
    return code;
}
