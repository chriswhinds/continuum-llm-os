#include "gateway_config.h"

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

int gateway_config_load(const char *path, gateway_config_t *out) {
    memset(out, 0, sizeof(*out));
    out->listen_port = 8080;
    strncpy(out->scheduler_unix_socket, "/run/continuum/schedulerd.sock", sizeof(out->scheduler_unix_socket) - 1);

    FILE *f = fopen(path, "r");
    if (!f) {
        clog_error("gateway_config: cannot open %s", path);
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') continue;
        char *cursor = trimmed;
        char *key = next_token(&cursor);
        if (!key) continue;

        if (strcmp(key, "listen_port") == 0) {
            char *v = next_token(&cursor);
            if (v) out->listen_port = (uint16_t)atoi(v);
        } else if (strcmp(key, "scheduler_unix_socket") == 0) {
            char *v = next_token(&cursor);
            if (v) strncpy(out->scheduler_unix_socket, v, sizeof(out->scheduler_unix_socket) - 1);
        } else if (strcmp(key, "model") == 0) {
            char *v = next_token(&cursor);
            if (v && out->n_models < GATEWAY_MAX_MODELS) {
                strncpy(out->models[out->n_models], v, sizeof(out->models[0]) - 1);
                out->n_models++;
            }
        } else {
            clog_warn("gateway_config: unknown directive '%s'", key);
        }
    }
    fclose(f);
    return 0;
}
