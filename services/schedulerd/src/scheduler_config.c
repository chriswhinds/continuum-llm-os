#include "scheduler_config.h"

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

int scheduler_config_load(const char *path, scheduler_config_t *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->listen_unix_socket, "/run/continuum/schedulerd.sock", sizeof(out->listen_unix_socket) - 1);

    FILE *f = fopen(path, "r");
    if (!f) {
        clog_error("scheduler_config: cannot open %s", path);
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') continue;
        char *cursor = trimmed;
        char *key = next_token(&cursor);
        if (!key) continue;

        if (strcmp(key, "listen_unix_socket") == 0) {
            char *v = next_token(&cursor);
            if (v) strncpy(out->listen_unix_socket, v, sizeof(out->listen_unix_socket) - 1);
        } else if (strcmp(key, "route") == 0) {
            char *name = next_token(&cursor);
            char *target = next_token(&cursor);
            if (name && target && out->n_routes < SCHEDULER_MAX_ROUTES) {
                char *colon = strrchr(target, ':');
                if (colon) {
                    *colon = '\0';
                    scheduler_route_t *r = &out->routes[out->n_routes];
                    strncpy(r->model_name, name, sizeof(r->model_name) - 1);
                    strncpy(r->shard_execd_host, target, sizeof(r->shard_execd_host) - 1);
                    r->shard_execd_port = (uint16_t)atoi(colon + 1);
                    out->n_routes++;
                } else {
                    clog_warn("scheduler_config: route target '%s' is not host:port", target);
                }
            }
        } else {
            clog_warn("scheduler_config: unknown directive '%s'", key);
        }
    }
    fclose(f);
    return 0;
}

const scheduler_route_t *scheduler_find_route(const scheduler_config_t *cfg, const char *model_name) {
    for (size_t i = 0; i < cfg->n_routes; i++) {
        if (strncmp(cfg->routes[i].model_name, model_name, sizeof(cfg->routes[i].model_name)) == 0) {
            return &cfg->routes[i];
        }
    }
    return NULL;
}
