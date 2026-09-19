/* membershipd -- ARCH-002 §05: node join/leave/health tracking for the
 * control-plane quorum, replicated via its own Raft group (lib/raftkv) --
 * kept separate from page-directoryd's group per ARCH-002 §05 so a
 * directory snapshot/compaction pause can never risk quorum stability.
 *
 * Client protocol: WIRE_MEMBERSHIP_QUERY / WIRE_MEMBERSHIP_UPDATE, both
 * answered with WIRE_MEMBERSHIP_RESP. A write on a non-leader node is
 * rejected with found=0 and node_id set to the current leader's node id.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "clog.h"
#include "raftkv.h"
#include "raftkv_daemon_config.h"
#include "sock_util.h"
#include "wire.h"

typedef struct {
    uint8_t status;
    uint64_t last_heartbeat_ns;
} membership_value_t;

static void handle_connection(raftkv_t *kv, int fd) {
    uint8_t type, flags;
    uint64_t stream_id;
    uint8_t buf[64];
    uint32_t paylen;
    if (wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) != 0) return;

    if (type == WIRE_MEMBERSHIP_QUERY && paylen >= sizeof(wire_membership_query_t)) {
        wire_membership_query_t req;
        memcpy(&req, buf, sizeof(req));

        wire_membership_resp_t resp = {.node_id = req.node_id};
        membership_value_t v;
        resp.found = raftkv_get(kv, req.node_id, &v) ? 1 : 0;
        if (resp.found) {
            resp.status = v.status;
            resp.last_heartbeat_ns = v.last_heartbeat_ns;
        }
        wire_send_frame_blocking(fd, WIRE_MEMBERSHIP_RESP, WIRE_FLAG_FINAL, stream_id, &resp, sizeof(resp));
    } else if (type == WIRE_MEMBERSHIP_UPDATE && paylen >= sizeof(wire_membership_resp_t)) {
        wire_membership_resp_t req;
        memcpy(&req, buf, sizeof(req));

        membership_value_t v = {.status = req.status, .last_heartbeat_ns = req.last_heartbeat_ns};
        int rc = raftkv_propose_set(kv, req.node_id, &v);

        wire_membership_resp_t resp = {.node_id = req.node_id};
        if (rc == 0) {
            resp.found = 1;
            resp.status = req.status;
            resp.last_heartbeat_ns = req.last_heartbeat_ns;
        } else {
            resp.found = 0;
            resp.node_id = (uint32_t)raftkv_leader_node_id(kv); /* redirect hint */
        }
        wire_send_frame_blocking(fd, WIRE_MEMBERSHIP_RESP, WIRE_FLAG_FINAL, stream_id, &resp, sizeof(resp));
    } else {
        clog_warn("membershipd: unexpected frame type %u", type);
    }
}

int main(int argc, char **argv) {
    clog_init("membershipd");
    const char *config_path = "/etc/continuum/membershipd.conf";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) clog_set_level(CLOG_DEBUG);
    }

    raftkv_daemon_config_t dcfg;
    if (raftkv_daemon_config_load(config_path, &dcfg) != 0) return 1;

    raftnet_config_t raft_cfg = {
        .my_node_id = dcfg.node_id,
        .peers = dcfg.peers,
        .n_peers = dcfg.n_peers,
        .election_timeout_ms = 1000,
        .request_timeout_ms = 200,
        .log_tag = "membershipd/raft",
    };
    raftkv_t *kv = raftkv_start(&raft_cfg, 1024, sizeof(membership_value_t));

    int listen_fd = sock_tcp_listen(NULL, dcfg.client_listen_port, 32);
    if (listen_fd < 0) {
        clog_error("membershipd: cannot listen on :%u: %s", dcfg.client_listen_port, strerror(errno));
        return 1;
    }
    int fl = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, fl & ~O_NONBLOCK);

    clog_info("membershipd: node %u ready, %zu peer(s) in the Raft group, client port :%u",
              dcfg.node_id, dcfg.n_peers, dcfg.client_listen_port);

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_connection(kv, fd);
        close(fd);
    }

    raftkv_stop(kv);
    return 0;
}
