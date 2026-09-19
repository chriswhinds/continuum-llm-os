/* node_agent_config.h -- config file format for node-agentd.
 *
 * node_id 7
 * listen_port 7201
 * page_size 4096
 * health_interval_ms 2000
 * telemetry_sink 10.0.0.10:7401
 */
#ifndef NODE_AGENT_CONFIG_H
#define NODE_AGENT_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define NODE_AGENT_MAX_SINKS 4

typedef struct {
    char host[64];
    uint16_t port;
} node_agent_sink_t;

typedef struct {
    uint32_t node_id;
    uint16_t listen_port;
    uint64_t page_size; /* must match this node's swapd.conf */
    int health_interval_ms;
    node_agent_sink_t sinks[NODE_AGENT_MAX_SINKS];
    size_t n_sinks;
} node_agent_config_t;

int node_agent_config_load(const char *path, node_agent_config_t *out);

#endif
