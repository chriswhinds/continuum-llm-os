#include "raftkv_daemon_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clog.h"

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = '\0';
    return s;
}

static char *next_token(char **cursor) {
    char *s = *cursor;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') { *cursor = s; return NULL; }
    char *start = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) { *s = '\0'; s++; }
    *cursor = s;
    return start;
}

int raftkv_daemon_config_load(const char *path, raftkv_daemon_config_t *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "r");
    if (!f) {
        clog_error("raftkv_daemon_config: cannot open %s", path);
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') continue;
        char *cursor = trimmed;
        char *key = next_token(&cursor);
        if (!key) continue;

        if (strcmp(key, "node_id") == 0) {
            char *v = next_token(&cursor);
            if (v) out->node_id = (uint32_t)strtoul(v, NULL, 10);
        } else if (strcmp(key, "client_listen_port") == 0) {
            char *v = next_token(&cursor);
            if (v) out->client_listen_port = (uint16_t)atoi(v);
        } else if (strcmp(key, "peer") == 0) {
            char *id = next_token(&cursor);
            char *host = next_token(&cursor);
            char *port = next_token(&cursor);
            if (id && host && port && out->n_peers < RAFTKV_DAEMON_MAX_PEERS) {
                raftnet_peer_t *p = &out->peers[out->n_peers];
                p->node_id = atoi(id);
                strncpy(p->host, host, sizeof(p->host) - 1);
                p->port = (uint16_t)atoi(port);
                out->n_peers++;
            }
        } else {
            clog_warn("raftkv_daemon_config: unknown directive '%s'", key);
        }
    }
    fclose(f);
    return 0;
}
