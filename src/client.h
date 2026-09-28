#ifndef PS_CLIENT_H
#define PS_CLIENT_H

#include "playsync2.h"

typedef struct {
    const char *host;
    int port;
    const char *name;
    int no_mpv;
    int show_drift;
    int allow_local_speed;
    char **mpv_args; /* passthrough, after "--" */
    int mpv_argc;
} client_config;

int client_run(const client_config *cfg);

#endif
