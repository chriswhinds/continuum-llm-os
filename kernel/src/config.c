#include "config.h"

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
    if (*s == '\0') {
        *cursor = s;
        return NULL;
    }
    char *start = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) {
        *s = '\0';
        s++;
    }
    *cursor = s;
    return start;
}

int config_load(const char *path, continuumd_config_t *out) {
    memset(out, 0, sizeof(*out));
    out->heartbeat_interval_ms = 2000;

    FILE *f = fopen(path, "r");
    if (!f) {
        clog_error("config: cannot open %s", path);
        return -1;
    }

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *trimmed = trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') continue;

        char *cursor = trimmed;
        char *keyword = next_token(&cursor);
        if (!keyword) continue;

        if (strcmp(keyword, "uart_device") == 0) {
            char *val = next_token(&cursor);
            if (val) strncpy(out->uart_device, val, sizeof(out->uart_device) - 1);
        } else if (strcmp(keyword, "heartbeat_interval_ms") == 0) {
            char *val = next_token(&cursor);
            if (val) out->heartbeat_interval_ms = atoi(val);
        } else if (strcmp(keyword, "service") == 0) {
            if (out->n_services >= CONTINUUMD_MAX_SERVICES) {
                clog_error("config: %s:%d too many services (max %d)", path, lineno, CONTINUUMD_MAX_SERVICES);
                continue;
            }
            service_spec_t *svc = &out->services[out->n_services];
            memset(svc, 0, sizeof(*svc));

            char *name = next_token(&cursor);
            char *exec = next_token(&cursor);
            if (!name || !exec) {
                clog_error("config: %s:%d malformed 'service' line", path, lineno);
                continue;
            }
            strncpy(svc->name, name, sizeof(svc->name) - 1);
            strncpy(svc->exec, exec, sizeof(svc->exec) - 1);
            svc->argv[0] = svc->exec;
            svc->argc = 1;

            char *arg;
            while ((arg = next_token(&cursor)) != NULL && svc->argc < CONTINUUMD_MAX_ARGS + 1) {
                /* argv entries must outlive this function: store them back
                 * into the line buffer's lifetime is NOT safe (stack reused
                 * next iteration), so duplicate. */
                svc->argv[svc->argc++] = strdup(arg);
            }
            svc->argv[svc->argc] = NULL;
            out->n_services++;
        } else {
            clog_warn("config: %s:%d unknown directive '%s'", path, lineno, keyword);
        }
    }

    fclose(f);
    clog_info("config: loaded %zu service(s) from %s", (size_t)out->n_services, path);
    return 0;
}
