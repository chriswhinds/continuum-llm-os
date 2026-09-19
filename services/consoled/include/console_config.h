/* console_config.h -- config file format for consoled.
 *
 * http_listen_port 7500
 * telemetry_listen_port 7401
 *
 * Point every node-agentd's `telemetry_sink` (node_agent_config.h) at
 * this process's telemetry_listen_port.
 */
#ifndef CONSOLE_CONFIG_H
#define CONSOLE_CONFIG_H

#include <stdint.h>

typedef struct {
    uint16_t http_listen_port;
    uint16_t telemetry_listen_port;
} console_config_t;

int console_config_load(const char *path, console_config_t *out);

#endif
