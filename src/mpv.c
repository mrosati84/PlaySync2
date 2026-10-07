#include "mpv.h"
#include "net.h"
#include "timebase.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define OBS_PAUSE 1
#define OBS_TIME_POS 2
#define OBS_SEEKING 3
#define OBS_EOF 4
#define OBS_CACHE 5
#define OBS_SPEED 6
#define OBS_IDLE 7
#define OBS_PLAYLIST 8

#define MPV_EVQ 128
#define MPV_SOCK_CONNECT_TIMEOUT 2.0

typedef struct {
    int active;
    double value;
    int reasserted;
    double deadline;
} pending_t;

struct mpv {
    int fd;
    pid_t pid;
    int exited;
    int child_reaped;
    int child_status;
    int verbose;
    char sockpath[256];
    lreader lr;
    obuf out;
    long req_id;

    int have_pause, pause;
    int have_time_pos;
    double time_pos;
    int have_seeking, seeking;
    int have_eof, eof_reached;
    int have_cache, paused_for_cache;
    int have_speed;
    double speed;
    int have_idle, idle_active;
    int have_playlist;
    int playlist_pos;
    int loaded;

    double engine_speed;
    double rtt;
    int allow_local_speed;

    pending_t p_pause;
    pending_t p_pos;
    pending_t p_speed;

    int seek_armed;
    double pre_seek_pos;

    int suspect_pause;
    double suspect_t;

    mpv_event evq[MPV_EVQ];
    int evhead;
    int evcount;
};

static void ev_push(mpv_t *m, mpv_ev_kind kind, int pause, double pos)
{
    if (m->evcount >= MPV_EVQ) {
        /* Drop the oldest to keep the most recent signal. */
        m->evhead = (m->evhead + 1) % MPV_EVQ;
        m->evcount--;
    }
    int idx = (m->evhead + m->evcount) % MPV_EVQ;
    m->evq[idx].kind = kind;
    m->evq[idx].pause = pause;
    m->evq[idx].pos = pos;
    m->evcount++;
}

int mpv_next_event(mpv_t *m, mpv_event *out)
{
    if (m->evcount <= 0)
        return 0;
    *out = m->evq[m->evhead];
    m->evhead = (m->evhead + 1) % MPV_EVQ;
    m->evcount--;
    return 1;
}

static void mpv_send_raw(mpv_t *m, const char *json)
{
    if (m->fd < 0)
        return;
    if (m->verbose)
        log_trace("mpv <- %s", json);
    int r = obuf_append_line(&m->out, json, strlen(json));
    if (r == -2)
        log_warn("mpv: player is not reading IPC commands; dropping a command");
    else if (r != 0)
        log_warn("mpv: out of memory queueing an IPC command");
    (void)obuf_flush(m->fd, &m->out);
}

static void mpv_send_obj(mpv_t *m, cJSON *root)
{
    if (root == NULL)
        return;
    cJSON_AddNumberToObject(root, "request_id", (double)(++m->req_id));
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        mpv_send_raw(m, s);
        free(s);
    }
    cJSON_Delete(root);
}

static void send_observe(mpv_t *m, int id, const char *name)
{
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("observe_property"));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(id));
    cJSON_AddItemToArray(arr, cJSON_CreateString(name));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "command", arr);
    mpv_send_obj(m, root);
}

static void raw_pause(mpv_t *m, int pause)
{
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("set_property"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("pause"));
    cJSON_AddItemToArray(arr, cJSON_CreateBool(pause ? 1 : 0));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "command", arr);
    mpv_send_obj(m, root);
}

static void raw_speed(mpv_t *m, double speed)
{
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("set_property"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("speed"));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(speed));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "command", arr);
    mpv_send_obj(m, root);
}

static void raw_seek(mpv_t *m, double pos)
{
    cJSON *arr = cJSON_CreateArray();
    if (m->have_pause && m->pause) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("set_property"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("time-pos"));
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(pos));
    } else {
        cJSON_AddItemToArray(arr, cJSON_CreateString("seek"));
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(pos));
        cJSON_AddItemToArray(arr, cJSON_CreateString("absolute"));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "command", arr);
    mpv_send_obj(m, root);
}

void mpv_cmd_pause(mpv_t *m, int pause)
{
    if (m->fd < 0)
        return;
    if (m->have_pause && m->pause == (pause ? 1 : 0))
        return;
    double deadline = tb_now() + (m->rtt * 3.0 > 0.5 ? m->rtt * 3.0 : 0.5);
    m->p_pause.active = 1;
    m->p_pause.value = pause ? 1.0 : 0.0;
    m->p_pause.reasserted = 0;
    m->p_pause.deadline = deadline;
    raw_pause(m, pause);
}

void mpv_cmd_speed(mpv_t *m, double speed)
{
    if (m->fd < 0)
        return;
    if (m->have_speed && fabs(m->speed - speed) < 1e-6)
        return;
    double deadline = tb_now() + (m->rtt * 3.0 > 0.5 ? m->rtt * 3.0 : 0.5);
    m->p_speed.active = 1;
    m->p_speed.value = speed;
    m->p_speed.reasserted = 0;
    m->p_speed.deadline = deadline;
    m->engine_speed = speed;
    raw_speed(m, speed);
}

void mpv_cmd_seek(mpv_t *m, double pos)
{
    if (m->fd < 0)
        return;
    /* Seeking to where MPV already is emits no property-change, so a pending
     * record would only expire into a spurious "not confirmed" warning. */
    if (m->have_time_pos && fabs(m->time_pos - pos) < 1e-3)
        return;
    double deadline = tb_now() + (m->rtt * 3.0 > 0.5 ? m->rtt * 3.0 : 0.5);
    m->p_pos.active = 1;
    m->p_pos.value = pos;
    m->p_pos.reasserted = 0;
    m->p_pos.deadline = deadline;
    raw_seek(m, pos);
}

void mpv_apply_pause(mpv_t *m, int pause, double pos)
{
    mpv_cmd_seek(m, pos);
    mpv_cmd_pause(m, pause);
}

void mpv_apply_seek(mpv_t *m, double pos)
{
    mpv_cmd_seek(m, pos);
}

void mpv_show_text(mpv_t *m, const char *text)
{
    if (m->fd < 0)
        return;
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("show-text"));
    cJSON_AddItemToArray(arr, cJSON_CreateString(text));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "command", arr);
    mpv_send_obj(m, root);
}

void mpv_set_rtt(mpv_t *m, double rtt)
{
    m->rtt = rtt;
}

void mpv_set_allow_local_speed(mpv_t *m, int allow)
{
    m->allow_local_speed = allow;
}

int mpv_fd(const mpv_t *m) { return m->fd; }
pid_t mpv_pid(const mpv_t *m) { return m->pid; }
int mpv_exited(const mpv_t *m) { return m->exited; }
int mpv_loaded(const mpv_t *m) { return m->loaded; }
int mpv_has_pos(const mpv_t *m) { return m->loaded && m->have_time_pos; }
double mpv_pos(const mpv_t *m) { return m->time_pos; }
int mpv_paused(const mpv_t *m) { return m->have_pause && m->pause; }
double mpv_speed(const mpv_t *m) { return m->have_speed ? m->speed : 1.0; }
double mpv_engine_speed(const mpv_t *m) { return m->engine_speed; }
int mpv_eof_reached(const mpv_t *m) { return m->have_eof && m->eof_reached; }

pstate mpv_state(const mpv_t *m)
{
    if (m->exited)
        return ST_IDLE;
    if (m->have_idle && m->idle_active)
        return ST_IDLE;
    if (!m->loaded)
        return ST_LOADING;
    if (m->have_eof && m->eof_reached)
        return ST_EOF;
    if (m->have_seeking && m->seeking)
        return ST_SEEKING;
    if (m->have_cache && m->paused_for_cache)
        return ST_BUFFERING;
    if (m->have_pause && m->pause)
        return ST_PAUSED;
    return ST_PLAYING;
}

static void resolve_suspect(mpv_t *m, double now)
{
    if (!m->suspect_pause)
        return;
    if (m->have_eof && m->eof_reached) {
        m->suspect_pause = 0;
        ev_push(m, MPV_EV_EOF, 1, m->time_pos);
    } else if (now - m->suspect_t >= 0.10) {
        m->suspect_pause = 0;
        ev_push(m, MPV_EV_PAUSE, 1, m->time_pos);
    }
}

void mpv_tick(mpv_t *m)
{
    if (m->fd < 0)
        return;
    double now = tb_now();
    double tb = m->rtt * 3.0 > 0.5 ? m->rtt * 3.0 : 0.5;

    pending_t *pend[3] = { &m->p_pause, &m->p_pos, &m->p_speed };
    for (int i = 0; i < 3; i++) {
        pending_t *p = pend[i];
        if (!p->active || now < p->deadline)
            continue;
        if (!p->reasserted) {
            p->reasserted = 1;
            p->deadline = now + tb;
            if (i == 0)
                raw_pause(m, (int)p->value);
            else if (i == 1)
                raw_seek(m, p->value);
            else
                raw_speed(m, p->value);
        } else {
            log_warn("mpv: command not confirmed before deadline");
            p->active = 0;
        }
    }
    resolve_suspect(m, now);
}

static void handle_property_change(mpv_t *m, const cJSON *root)
{
    const cJSON *iditem = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *nameitem = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    int id = cJSON_IsNumber(iditem) ? (int)iditem->valuedouble : 0;
    const char *name = (cJSON_IsString(nameitem) && nameitem->valuestring) ? nameitem->valuestring : "";
    if (id == 0) {
        if (strcmp(name, "pause") == 0) id = OBS_PAUSE;
        else if (strcmp(name, "time-pos") == 0) id = OBS_TIME_POS;
        else if (strcmp(name, "seeking") == 0) id = OBS_SEEKING;
        else if (strcmp(name, "eof-reached") == 0) id = OBS_EOF;
        else if (strcmp(name, "paused-for-cache") == 0) id = OBS_CACHE;
        else if (strcmp(name, "speed") == 0) id = OBS_SPEED;
        else if (strcmp(name, "idle-active") == 0) id = OBS_IDLE;
        else if (strcmp(name, "playlist-pos") == 0) id = OBS_PLAYLIST;
    }

    switch (id) {
    case OBS_PAUSE: {
        if (!cJSON_IsBool(data)) {
            m->have_pause = 0;
            break;
        }
        int v = cJSON_IsTrue(data) ? 1 : 0;
        m->have_pause = 1;
        m->pause = v;
        if (m->p_pause.active && (int)m->p_pause.value == v) {
            m->p_pause.active = 0;
            break;
        }
        m->p_pause.active = 0;
        if (v) {
            if (m->have_eof && m->eof_reached) {
                ev_push(m, MPV_EV_EOF, 1, m->time_pos);
            } else {
                m->suspect_pause = 1;
                m->suspect_t = tb_now();
            }
        } else {
            ev_push(m, MPV_EV_PAUSE, 0, m->time_pos);
        }
        break;
    }
    case OBS_TIME_POS: {
        if (!cJSON_IsNumber(data)) {
            m->have_time_pos = 0;
            break;
        }
        double nv = data->valuedouble;
        double old = m->time_pos;
        m->have_time_pos = 1;
        m->time_pos = nv;
        if (m->p_pos.active) {
            m->p_pos.active = 0;
            m->seek_armed = 0;
            break;
        }
        if (m->seek_armed && fabs(nv - m->pre_seek_pos) > 1.0) {
            m->seek_armed = 0;
            ev_push(m, MPV_EV_SEEK, 0, nv);
        } else if (m->have_time_pos && old - nv > 1.0 &&
                   (m->have_seeking && m->seeking)) {
            ev_push(m, MPV_EV_SEEK, 0, nv);
        }
        break;
    }
    case OBS_SEEKING:
        if (cJSON_IsBool(data)) {
            m->have_seeking = 1;
            m->seeking = cJSON_IsTrue(data) ? 1 : 0;
        } else {
            m->have_seeking = 0;
        }
        break;
    case OBS_EOF:
        if (cJSON_IsBool(data)) {
            m->have_eof = 1;
            int v = cJSON_IsTrue(data) ? 1 : 0;
            if (v && !m->eof_reached && m->have_pause && m->pause)
                ev_push(m, MPV_EV_EOF, 1, m->time_pos);
            m->eof_reached = v;
        } else {
            m->have_eof = 0;
        }
        break;
    case OBS_CACHE:
        if (cJSON_IsBool(data)) {
            m->have_cache = 1;
            m->paused_for_cache = cJSON_IsTrue(data) ? 1 : 0;
        } else {
            m->have_cache = 0;
        }
        break;
    case OBS_SPEED: {
        if (!cJSON_IsNumber(data)) {
            m->have_speed = 0;
            break;
        }
        double nv = data->valuedouble;
        m->have_speed = 1;
        m->speed = nv;
        if (m->p_speed.active && fabs(m->p_speed.value - nv) < 1e-6) {
            m->p_speed.active = 0;
            break;
        }
        m->p_speed.active = 0;
        if (m->allow_local_speed) {
            m->engine_speed = nv;
        } else {
            ev_push(m, MPV_EV_SPEED, 0, nv);
        }
        break;
    }
    case OBS_IDLE:
        if (cJSON_IsBool(data)) {
            m->have_idle = 1;
            m->idle_active = cJSON_IsTrue(data) ? 1 : 0;
            if (m->idle_active)
                m->loaded = 0;
            else
                m->loaded = 1;
        } else {
            m->have_idle = 0;
        }
        break;
    case OBS_PLAYLIST:
        if (cJSON_IsNumber(data)) {
            int v = (int)data->valuedouble;
            if (m->have_playlist && v != m->playlist_pos)
                ev_push(m, MPV_EV_START, 0, 0);
            m->have_playlist = 1;
            m->playlist_pos = v;
            if (v >= 0)
                m->loaded = 1;
        } else {
            m->have_playlist = 0;
        }
        break;
    default:
        break;
    }
}

static void handle_event(mpv_t *m, const cJSON *root)
{
    const cJSON *evitem = cJSON_GetObjectItemCaseSensitive(root, "event");
    const char *ev = (cJSON_IsString(evitem) && evitem->valuestring) ? evitem->valuestring : "";
    if (strcmp(ev, "property-change") == 0) {
        handle_property_change(m, root);
    } else if (strcmp(ev, "seek") == 0) {
        m->seek_armed = 1;
        m->pre_seek_pos = m->time_pos;
    } else if (strcmp(ev, "file-loaded") == 0) {
        m->loaded = 1;
        m->have_idle = 1;
        m->idle_active = 0;
        ev_push(m, MPV_EV_LOADED, 0, 0);
    } else if (strcmp(ev, "start-file") == 0) {
        m->loaded = 0;
        m->p_pos.active = m->p_pause.active = m->p_speed.active = 0;
        m->seek_armed = 0;
        m->suspect_pause = 0;
        ev_push(m, MPV_EV_START, 0, 0);
    } else if (strcmp(ev, "playback-restart") == 0) {
        /* Re-arm the classifier; the achieved position arrives next. */
        m->seek_armed = 0;
    } else if (strcmp(ev, "end-file") == 0) {
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
        const char *r = (cJSON_IsString(reason) && reason->valuestring) ? reason->valuestring : "";
        if (strcmp(r, "quit") == 0 || strcmp(r, "error") == 0 || strcmp(r, "eof") == 0) {
            if (strcmp(r, "quit") == 0)
                m->exited = 1;
        }
    }
}

static void handle_reply(mpv_t *m, const cJSON *root)
{
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    if (cJSON_IsString(err) && err->valuestring && strcmp(err->valuestring, "success") != 0) {
        log_warn("mpv: command rejected: %s (request_id %ld)", err->valuestring,
                 cJSON_IsNumber(rid) ? (long)rid->valuedouble : -1L);
    }
    (void)m;
}

static void handle_line(mpv_t *m, const char *line, size_t len)
{
    if (len == 0)
        return;
    if (m->verbose)
        log_trace("mpv -> %s", line);
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (root == NULL) {
        log_warn("mpv: unparseable line");
        return;
    }
    if (cJSON_GetObjectItemCaseSensitive(root, "event") != NULL)
        handle_event(m, root);
    else
        handle_reply(m, root);
    cJSON_Delete(root);
}

int mpv_poll(mpv_t *m)
{
    if (m->fd < 0)
        return 0;
    for (;;) {
        ssize_t r = lr_fill(m->fd, &m->lr);
        if (r > 0) {
            char *line;
            size_t len;
            int lr;
            while ((lr = lr_next(&m->lr, &line, &len)) == 1)
                handle_line(m, line, len);
            if (lr < 0) {
                log_error("mpv: message exceeds the 64 KiB cap");
                return -1;
            }
        } else if (r == 0) {
            m->exited = 1;
            return -1;
        } else if (r == -1) {
            m->exited = 1;
            return -1;
        } else {
            break;
        }
    }
    resolve_suspect(m, tb_now());
    return 0;
}

int mpv_pending(const mpv_t *m)
{
    return obuf_pending(&m->out);
}

void mpv_flush(mpv_t *m)
{
    if (m->fd >= 0)
        (void)obuf_flush(m->fd, &m->out);
}

int mpv_child_status(mpv_t *m, int *status)
{
    if (m->child_reaped) {
        if (status)
            *status = m->child_status;
        return 1;
    }
    if (m->pid <= 0)
        return 0;
    int st = 0;
    pid_t r = waitpid(m->pid, &st, WNOHANG);
    if (r == m->pid) {
        m->child_reaped = 1;
        m->child_status = st;
        m->exited = 1;
        if (status)
            *status = st;
        return 1;
    }
    return 0;
}

static int wait_for_socket(mpv_t *m, double timeout)
{
    double start = tb_now();
    for (;;) {
        int fd = net_unix_connect(m->sockpath);
        if (fd >= 0)
            return fd;
        int st = 0;
        if (mpv_child_status(m, &st))
            return -1;
        if (tb_now() - start > timeout)
            return -1;
        struct pollfd dummy;
        dummy.fd = -1;
        dummy.events = 0;
        dummy.revents = 0;
        (void)poll(&dummy, 0, 25);
    }
}

mpv_t *mpv_spawn(const char *socket_path, char *const *passthrough, int npass,
                 int verbose, char *err, size_t errlen)
{
    const char *mpvbin = getenv("PLAYSYNC2_MPV");
    if (mpvbin == NULL || *mpvbin == '\0')
        mpvbin = "mpv";

    int injected = 3;
    int total = 1 + npass + injected;
    char **argv = calloc((size_t)total + 1, sizeof(char *));
    if (!argv) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    int argc = 0;
    argv[argc++] = (char *)mpvbin;
    for (int i = 0; i < npass; i++)
        argv[argc++] = passthrough[i];
    char opt_ipc[512];
    snprintf(opt_ipc, sizeof(opt_ipc), "--input-ipc-server=%s", socket_path);
    char opt_keep[] = "--keep-open=yes";
    char opt_pause[] = "--pause=yes";
    argv[argc++] = opt_ipc;
    argv[argc++] = opt_keep;
    argv[argc++] = opt_pause;
    argv[argc] = NULL;

    /* Log the exact command line locally (SPEC §11.3). */
    for (int i = 0; i < argc; i++)
        log_info("mpv argv[%d]=%s", i, argv[i]);

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errlen, "fork failed: %s", strerror(errno));
        free(argv);
        return NULL;
    }
    if (pid == 0) {
        execvp(mpvbin, argv);
        _exit(127);
    }
    free(argv);

    mpv_t *m = calloc(1, sizeof(*m));
    if (!m) {
        kill(pid, SIGKILL);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    m->fd = -1;
    m->pid = pid;
    m->verbose = verbose;
    snprintf(m->sockpath, sizeof(m->sockpath), "%s", socket_path);
    m->engine_speed = 1.0;
    m->rtt = 0.05;
    lr_init(&m->lr);
    obuf_init(&m->out);

    int fd = wait_for_socket(m, MPV_SOCK_CONNECT_TIMEOUT);
    if (fd < 0) {
        int st = 0;
        int exited = mpv_child_status(m, &st);
        snprintf(err, errlen, exited ? "mpv exited before its IPC socket appeared"
                                     : "mpv did not create its IPC socket within 2 s");
        mpv_destroy(m, 1);
        return NULL;
    }
    m->fd = fd;

    send_observe(m, OBS_PAUSE, "pause");
    send_observe(m, OBS_TIME_POS, "time-pos");
    send_observe(m, OBS_SEEKING, "seeking");
    send_observe(m, OBS_EOF, "eof-reached");
    send_observe(m, OBS_CACHE, "paused-for-cache");
    send_observe(m, OBS_SPEED, "speed");
    send_observe(m, OBS_IDLE, "idle-active");
    send_observe(m, OBS_PLAYLIST, "playlist-pos");
    return m;
}

void mpv_destroy(mpv_t *m, int kill_it)
{
    if (!m)
        return;
    if (m->fd >= 0) {
        net_close(m->fd);
        m->fd = -1;
    }
    if (kill_it && m->pid > 0 && !m->child_reaped) {
        kill(m->pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 40; i++) {
            pid_t r = waitpid(m->pid, &status, WNOHANG);
            if (r == m->pid) {
                m->child_reaped = 1;
                break;
            }
            struct pollfd dummy;
            dummy.fd = -1;
            dummy.events = 0;
            dummy.revents = 0;
            (void)poll(&dummy, 0, 50);
        }
        if (!m->child_reaped) {
            kill(m->pid, SIGKILL);
            (void)waitpid(m->pid, &status, 0);
            m->child_reaped = 1;
        }
    }
    if (m->sockpath[0])
        (void)unlink(m->sockpath);
    lr_free(&m->lr);
    obuf_free(&m->out);
    free(m);
}
