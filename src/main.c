#include "client.h"
#include "net.h"
#include "playsync2.h"
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int g_log_level = 1;

static void vlog(const char *level, const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("error", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("warn", fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...)
{
    if (g_log_level < 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    vlog("info", fmt, ap);
    va_end(ap);
}

void log_trace(const char *fmt, ...)
{
    if (g_log_level < 2)
        return;
    va_list ap;
    va_start(ap, fmt);
    vlog("trace", fmt, ap);
    va_end(ap);
}

void ps_uuid4(char *out, size_t outlen)
{
    if (outlen < 37)
        return;
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, b, sizeof(b));
        close(fd);
        if (r != (ssize_t)sizeof(b))
            fd = -1;
    }
    if (fd < 0) {
        for (size_t i = 0; i < sizeof(b); i++)
            b[i] = (unsigned char)(rand() & 0xff);
    }
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    snprintf(out, outlen,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
             b[11], b[12], b[13], b[14], b[15]);
}

static void usage(FILE *f)
{
    fprintf(f,
        PS_VERSION_STR "\n"
        "\n"
        "Usage:\n"
        "  playsync2 server [options]\n"
        "  playsync2 client --connect HOST:PORT [options] [-- mpv args...]\n"
        "  playsync2 --version | --help\n"
        "\n"
        "Server options:\n"
        "  --bind ADDR          listen address            (default 0.0.0.0)\n"
        "  --port N             listen port               (default %d)\n"
        "  --name NAME          server name for its logs  (default hostname)\n"
        "  --max-members N      refuse beyond N            (default %d, ceiling %d)\n"
        "  --exit-when-empty    exit when the last member leaves\n"
        "  --verbose | --quiet\n"
        "\n"
        "Client options:\n"
        "  --connect HOST:PORT  server to connect to (required)\n"
        "  --name NAME          member display name        (default $USER@hostname)\n"
        "  --no-mpv             observer only; no MPV is spawned\n"
        "  --show-drift         show local drift as an MPV OSD overlay\n"
        "  --allow-local-speed  do not revert local speed changes (debug)\n"
        "  --verbose | --quiet\n",
        PS_DEFAULT_PORT, PS_MAX_MEMBERS_DEFAULT, PS_MAX_MEMBERS_CEIL);
}

static const char *default_name(void)
{
    /* Headroom over the wire field (PS_NAME_LEN) so the composed
     * "user@host" is not silently clipped before it is copied into the
     * fixed-size proto name field. */
    static char buf[PS_NAME_LEN + 128];
    const char *user = getenv("USER");
    if (!user)
        user = getenv("LOGNAME");
    if (!user)
        user = "user";
    char host[128] = "host";
    (void)gethostname(host, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    snprintf(buf, sizeof(buf), "%s@%s", user, host);
    return buf;
}

static const char *hostname_only(void)
{
    static char host[128] = "host";
    (void)gethostname(host, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    return host;
}

static int parse_long(const char *s, long *out)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    *out = v;
    return 0;
}

static int cmd_server(int argc, char **argv)
{
    server_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "0.0.0.0";
    cfg.port = PS_DEFAULT_PORT;
    cfg.name = hostname_only();
    cfg.max_members = PS_MAX_MEMBERS_DEFAULT;
    cfg.exit_when_empty = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--verbose") == 0) {
            g_log_level = 2;
        } else if (strcmp(a, "--quiet") == 0) {
            g_log_level = 0;
        } else if (strcmp(a, "--exit-when-empty") == 0) {
            cfg.exit_when_empty = 1;
        } else if (strncmp(a, "--bind=", 7) == 0) {
            cfg.bind_addr = a + 7;
        } else if (strcmp(a, "--bind") == 0 && i + 1 < argc) {
            cfg.bind_addr = argv[++i];
        } else if (strncmp(a, "--port=", 7) == 0) {
            long v;
            if (parse_long(a + 7, &v) != 0 || v <= 0 || v > 65535) {
                fprintf(stderr, "playsync2: invalid --port\n");
                return 2;
            }
            cfg.port = (int)v;
        } else if (strcmp(a, "--port") == 0 && i + 1 < argc) {
            long v;
            if (parse_long(argv[++i], &v) != 0 || v <= 0 || v > 65535) {
                fprintf(stderr, "playsync2: invalid --port\n");
                return 2;
            }
            cfg.port = (int)v;
        } else if (strncmp(a, "--name=", 7) == 0) {
            cfg.name = a + 7;
        } else if (strcmp(a, "--name") == 0 && i + 1 < argc) {
            cfg.name = argv[++i];
        } else if (strncmp(a, "--max-members=", 14) == 0) {
            long v;
            if (parse_long(a + 14, &v) != 0 || v < 1) {
                fprintf(stderr, "playsync2: invalid --max-members\n");
                return 2;
            }
            cfg.max_members = (int)(v > PS_MAX_MEMBERS_CEIL ? PS_MAX_MEMBERS_CEIL : v);
        } else if (strcmp(a, "--max-members") == 0 && i + 1 < argc) {
            long v;
            if (parse_long(argv[++i], &v) != 0 || v < 1) {
                fprintf(stderr, "playsync2: invalid --max-members\n");
                return 2;
            }
            cfg.max_members = (int)(v > PS_MAX_MEMBERS_CEIL ? PS_MAX_MEMBERS_CEIL : v);
        } else {
            fprintf(stderr, "playsync2: unknown server option '%s' (try --help)\n", a);
            return 2;
        }
    }
    return server_run(&cfg);
}

static int cmd_client(int argc, char **argv)
{
    client_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = default_name();
    char host[256] = "";
    int have_connect = 0;
    int i = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--verbose") == 0) {
            g_log_level = 2;
        } else if (strcmp(a, "--quiet") == 0) {
            g_log_level = 0;
        } else if (strcmp(a, "--no-mpv") == 0) {
            cfg.no_mpv = 1;
        } else if (strcmp(a, "--show-drift") == 0) {
            cfg.show_drift = 1;
        } else if (strcmp(a, "--allow-local-speed") == 0) {
            cfg.allow_local_speed = 1;
        } else if (strncmp(a, "--name=", 7) == 0) {
            cfg.name = a + 7;
        } else if (strcmp(a, "--name") == 0 && i + 1 < argc) {
            cfg.name = argv[++i];
        } else if (strncmp(a, "--connect=", 10) == 0) {
            if (net_parse_hostport(a + 10, host, sizeof(host), &cfg.port) != 0) {
                fprintf(stderr, "playsync2: invalid --connect (want HOST:PORT)\n");
                return 2;
            }
            have_connect = 1;
        } else if (strcmp(a, "--connect") == 0 && i + 1 < argc) {
            if (net_parse_hostport(argv[++i], host, sizeof(host), &cfg.port) != 0) {
                fprintf(stderr, "playsync2: invalid --connect (want HOST:PORT)\n");
                return 2;
            }
            have_connect = 1;
        } else {
            fprintf(stderr, "playsync2: unknown client option '%s' (try --help)\n", a);
            return 2;
        }
    }
    if (!have_connect) {
        fprintf(stderr, "playsync2: client requires --connect HOST:PORT\n");
        return 2;
    }
    cfg.host = host;
    cfg.mpv_args = &argv[i];
    cfg.mpv_argc = argc - i;
    if (cfg.allow_local_speed)
        fprintf(stderr, "playsync2: warning: --allow-local-speed breaks synchronization\n");
    return client_run(&cfg);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("%s\n", PS_VERSION_STR);
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "server") == 0)
        return cmd_server(argc - 2, argv + 2);
    if (strcmp(cmd, "client") == 0)
        return cmd_client(argc - 2, argv + 2);
    fprintf(stderr, "playsync2: unknown subcommand '%s' (try --help)\n", cmd);
    return 2;
}
