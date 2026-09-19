/* raftkv_daemon_config.h -- the config file shape page-directoryd and
 * membershipd both use (ARCH-002 §05's two control-plane Raft groups):
 *
 * node_id 1
 * peer 1 127.0.0.1 17801
 * peer 2 127.0.0.1 17802
 * peer 3 127.0.0.1 17803
 * client_listen_port 7100
 *
 * `peer` lines must be identical across all three control-plane nodes'
 * config files (every node needs the whole cluster's roster to dial out
 * to the peers with a higher node_id -- see raftnet.h); only `node_id`
 * (which one *this* process is) and `client_listen_port` differ per node.
 */
#ifndef CONTINUUM_RAFTKV_DAEMON_CONFIG_H
#define CONTINUUM_RAFTKV_DAEMON_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "raftnet.h"

#define RAFTKV_DAEMON_MAX_PEERS 8

typedef struct {
    uint32_t node_id;
    raftnet_peer_t peers[RAFTKV_DAEMON_MAX_PEERS];
    size_t n_peers;
    uint16_t client_listen_port;
} raftkv_daemon_config_t;

int raftkv_daemon_config_load(const char *path, raftkv_daemon_config_t *out);

#endif
