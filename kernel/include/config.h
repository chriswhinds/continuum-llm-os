/* config.h -- continuumd's own tiny config file format.
 *
 * # comment
 * uart_device /dev/ttyACM0
 * heartbeat_interval_ms 2000
 * service node-agentd /opt/continuum/bin/node-agentd --config /etc/continuum/node-agentd.conf
 * service swapd       /opt/continuum/bin/swapd       --config /etc/continuum/swapd.conf
 *
 * `uart_device` is optional -- if absent, or if the device can't be opened
 * (e.g. this dev/test box has no boot processor attached), continuumd logs
 * a warning once and simply runs without a heartbeat rather than treating
 * it as fatal.
 */
#ifndef CONTINUUMD_CONFIG_H
#define CONTINUUMD_CONFIG_H

#include <stddef.h>

#define CONTINUUMD_MAX_SERVICES 16
#define CONTINUUMD_MAX_ARGS 16

typedef struct {
    char name[32];
    char exec[256];
    char *argv[CONTINUUMD_MAX_ARGS + 2]; /* argv[0]=exec, ..., NULL-terminated */
    int argc;
} service_spec_t;

typedef struct {
    char uart_device[128];
    int heartbeat_interval_ms;
    service_spec_t services[CONTINUUMD_MAX_SERVICES];
    size_t n_services;
} continuumd_config_t;

/* Returns 0 on success, -1 if the file couldn't be read or was malformed
 * (a diagnostic is logged either way via clog). */
int config_load(const char *path, continuumd_config_t *out);

#endif
