#include "swapd_config.h"

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

int swapd_config_load(const char *path, swapd_config_t *out) {
    memset(out, 0, sizeof(*out));
    out->page_size = 2 * 1024 * 1024; /* 2MB, matches ARCH-002's hugepage-sized default */
    out->high_watermark_pct = 85;
    strncpy(out->unix_socket, "/run/continuum/swapd.sock", sizeof(out->unix_socket) - 1);

    FILE *f = fopen(path, "r");
    if (!f) {
        clog_error("swapd_config: cannot open %s", path);
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
        } else if (strcmp(key, "tensor_region_bytes") == 0) {
            char *v = next_token(&cursor);
            if (v) out->tensor_region_bytes = strtoull(v, NULL, 10);
        } else if (strcmp(key, "page_size") == 0) {
            char *v = next_token(&cursor);
            if (v) out->page_size = strtoull(v, NULL, 10);
        } else if (strcmp(key, "dram_capacity_pages") == 0) {
            char *v = next_token(&cursor);
            if (v) out->dram_capacity_pages = strtoull(v, NULL, 10);
        } else if (strcmp(key, "unix_socket") == 0) {
            char *v = next_token(&cursor);
            if (v) strncpy(out->unix_socket, v, sizeof(out->unix_socket) - 1);
        } else if (strcmp(key, "high_watermark_pct") == 0) {
            char *v = next_token(&cursor);
            if (v) out->high_watermark_pct = atoi(v);
        } else if (strcmp(key, "remote_peer") == 0) {
            char *v = next_token(&cursor);
            if (v && out->n_peers < SWAPD_MAX_PEERS) {
                char *colon = strrchr(v, ':');
                if (colon) {
                    *colon = '\0';
                    strncpy(out->peers[out->n_peers].host, v, sizeof(out->peers[0].host) - 1);
                    out->peers[out->n_peers].port = (uint16_t)atoi(colon + 1);
                    out->n_peers++;
                }
            }
        } else {
            clog_warn("swapd_config: unknown directive '%s'", key);
        }
    }
    fclose(f);

    if (out->tensor_region_bytes == 0) {
        clog_error("swapd_config: tensor_region_bytes must be set");
        return -1;
    }
    if (out->tensor_region_bytes % out->page_size != 0) {
        clog_error("swapd_config: tensor_region_bytes must be a multiple of page_size");
        return -1;
    }
    if (out->dram_capacity_pages == 0) {
        out->dram_capacity_pages = out->tensor_region_bytes / out->page_size;
    }
    return 0;
}
