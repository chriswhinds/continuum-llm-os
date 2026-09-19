/* shard_config.h -- config file format for shard-execd.
 *
 * node_id 7
 * weight_file /var/lib/continuum/shard-07.weights
 * page_size 4096
 * swapd_unix_socket /run/continuum/swapd.sock
 * listen_port 7301
 * default_max_new_tokens 64
 * default_temperature 0.8
 *
 * swapd_unix_socket stays a Unix socket -- swapd and shard-execd always
 * run on the same node (ARCH-002 §01), so a local socket is the right
 * choice there. The dispatch listener is TCP, not Unix: schedulerd runs
 * on a control-plane node, shard-execd on a compute node -- different
 * physical boards per ARCH-002 §01 -- and a Unix socket can never be
 * reached across that boundary. (An earlier version of this file used a
 * Unix socket here too, which only ever worked because every e2e test
 * colocates every daemon in one process tree; nothing in a real
 * multi-node deployment could have used it.)
 */
#ifndef SHARD_CONFIG_H
#define SHARD_CONFIG_H

#include <stdint.h>

typedef struct {
    uint32_t node_id;
    char weight_file[256];
    uint64_t page_size;
    char swapd_unix_socket[128];
    uint16_t listen_port;
    uint32_t default_max_new_tokens;
    float default_temperature;
} shard_config_t;

int shard_config_load(const char *path, shard_config_t *out);

#endif
