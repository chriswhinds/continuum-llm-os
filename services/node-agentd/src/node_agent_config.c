#include "node_agent_config.h"

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

int node_agent_config_load(const char *path, node_agent_config_t *out) {
    memset(out, 0, sizeof(*out));
    out->page_size = 2 * 1024 * 1024;
    out->health_interval_ms = 2000;
    out->listen_port = 7201;

    FILE *f = fopen(path, "r");
    if (!f) {
        clog_error("node_agent_config: cannot open %s", path);
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
        } else if (strcmp(key, "listen_port") == 0) {
            char *v = next_token(&cursor);
            if (v) out->listen_port = (uint16_t)atoi(v);
        } else if (strcmp(key, "page_size") == 0) {
            char *v = next_token(&cursor);
            if (v) out->page_size = strtoull(v, NULL, 10);
        } else if (strcmp(key, "health_interval_ms") == 0) {
            char *v = next_token(&cursor);
            if (v) out->health_interval_ms = atoi(v);
        } else if (strcmp(key, "telemetry_sink") == 0) {
            char *v = next_token(&cursor);
            if (v && out->n_sinks < NODE_AGENT_MAX_SINKS) {
                char *colon = strrchr(v, ':');
                if (colon) {
                    *colon = '\0';
                    strncpy(out->sinks[out->n_sinks].host, v, sizeof(out->sinks[0].host) - 1);
                    out->sinks[out->n_sinks].port = (uint16_t)atoi(colon + 1);
                    out->n_sinks++;
                }
            }
        } else {
            clog_warn("node_agent_config: unknown directive '%s'", key);
        }
    }
    fclose(f);
    return 0;
}
