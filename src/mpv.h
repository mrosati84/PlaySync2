#ifndef PS_MPV_H
#define PS_MPV_H

#include "playsync2.h"

#include <sys/types.h>

typedef enum {
    MPV_EV_NONE = 0,
    MPV_EV_PAUSE,   /* local unexpected pause/resume -> intent */
    MPV_EV_SEEK,    /* local unexpected seek -> intent */
    MPV_EV_EOF,     /* eof reached -> pause intent at the group minimum */
    MPV_EV_SPEED,   /* local speed change that must be reverted */
    MPV_EV_LOADED,  /* file-loaded */
    MPV_EV_START    /* start-file / playlist advance -> rejoin */
} mpv_ev_kind;

typedef struct {
    mpv_ev_kind kind;
    int pause;
    double pos;
} mpv_event;

typedef struct mpv mpv_t;

mpv_t *mpv_spawn(const char *socket_path, char *const *passthrough, int npass,
                 int verbose, char *err, size_t errlen);
/* Close the IPC socket and remove the socket path. If kill_it, kill+reap. */
void mpv_destroy(mpv_t *m, int kill_it);

int mpv_fd(const mpv_t *m);
pid_t mpv_pid(const mpv_t *m);
int mpv_exited(const mpv_t *m);

int mpv_loaded(const mpv_t *m);
pstate mpv_state(const mpv_t *m);
int mpv_has_pos(const mpv_t *m);
double mpv_pos(const mpv_t *m);
int mpv_paused(const mpv_t *m);
double mpv_speed(const mpv_t *m);
double mpv_engine_speed(const mpv_t *m);
int mpv_eof_reached(const mpv_t *m);

void mpv_set_rtt(mpv_t *m, double rtt);
void mpv_set_allow_local_speed(mpv_t *m, int allow);

int mpv_pending(const mpv_t *m);
void mpv_flush(mpv_t *m);

/* Read and process available input. 0 = ok, -1 = fatal socket error. */
int mpv_poll(mpv_t *m);
int mpv_next_event(mpv_t *m, mpv_event *out);

void mpv_cmd_pause(mpv_t *m, int pause);
void mpv_cmd_speed(mpv_t *m, double speed);
void mpv_cmd_seek(mpv_t *m, double pos);
void mpv_apply_pause(mpv_t *m, int pause, double pos);
void mpv_apply_seek(mpv_t *m, double pos);
void mpv_show_text(mpv_t *m, const char *text);

/* Pending-deadline re-assert (4 Hz) and deferred EOF/pause resolution. */
void mpv_tick(mpv_t *m);

/* Reap the child if it has exited. Returns 1 and sets *status when reaped. */
int mpv_child_status(mpv_t *m, int *status);

#endif
