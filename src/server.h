#ifndef PS_SERVER_H
#define PS_SERVER_H

#include "playsync2.h"

typedef struct {
    const char *bind_addr;
    int port;
    const char *name;
    int max_members;
    int exit_when_empty;
} server_config;

int server_run(const server_config *cfg);

#endif
