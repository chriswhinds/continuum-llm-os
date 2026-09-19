#include "shard_config.h"

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

int shard_config_load(const char *path, shard_config_t *out) {
    memset(out, 0, sizeof(*out));
    out->page_size = 4096;
    out->default_max_new_tokens = 64;
    out->default_temperature = 0.8f;
    strncpy(out->swapd_unix_socket, "/run/continuum/swapd.sock", sizeof(out->swapd_unix_socket) - 1);
    out->listen_port = 7301;

    FILE *f = fopen(path, "r");
    if (!f) {
        clog_error("shard_config: cannot open %s", path);
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
        } else if (strcmp(key, "weight_file") == 0) {
            char *v = next_token(&cursor);
            if (v) strncpy(out->weight_file, v, sizeof(out->weight_file) - 1);
        } else if (strcmp(key, "page_size") == 0) {
            char *v = next_token(&cursor);
            if (v) out->page_size = strtoull(v, NULL, 10);
        } else if (strcmp(key, "swapd_unix_socket") == 0) {
            char *v = next_token(&cursor);
            if (v) strncpy(out->swapd_unix_socket, v, sizeof(out->swapd_unix_socket) - 1);
        } else if (strcmp(key, "listen_port") == 0) {
            char *v = next_token(&cursor);
            if (v) out->listen_port = (uint16_t)atoi(v);
        } else if (strcmp(key, "default_max_new_tokens") == 0) {
            char *v = next_token(&cursor);
            if (v) out->default_max_new_tokens = (uint32_t)atoi(v);
        } else if (strcmp(key, "default_temperature") == 0) {
            char *v = next_token(&cursor);
            if (v) out->default_temperature = (float)atof(v);
        } else {
            clog_warn("shard_config: unknown directive '%s'", key);
        }
    }
    fclose(f);
    return 0;
}
