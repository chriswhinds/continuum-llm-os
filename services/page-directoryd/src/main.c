/* page-directoryd -- ARCH-002 §05: the cluster-wide page_id -> {owner_
 * node, tier} directory, replicated via its own Raft group (lib/raftkv).
 *
 * Scope note: this reference build's swapd/node-agentd track residency
 * locally (their shared page table, lib/pagetable) but do not yet push
 * updates here -- see swapd's module comment. page-directoryd is a fully
 * real, independently-tested Raft-backed service (raftkv_test, and the
 * end-to-end test alongside this file), just not yet wired into the
 * paging hot path; doing so is a client added to swapd, not a change to
 * this daemon.
 *
 * Client protocol: WIRE_DIRECTORY_QUERY / WIRE_DIRECTORY_UPDATE, both
 * answered with WIRE_DIRECTORY_RESP (wire.h). A write on a non-leader
 * node is rejected with found=0 and owner_node_id set to the current
 * leader's node id, so the caller knows where to retry.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "clog.h"
#include "raftkv.h"
#include "raftkv_daemon_config.h"
#include "sock_util.h"
#include "wire.h"

typedef struct {
    uint32_t owner_node_id;
    uint8_t tier;
} pagedir_value_t;

static void handle_connection(raftkv_t *kv, int fd) {
    uint8_t type, flags;
    uint64_t stream_id;
    uint8_t buf[64];
    uint32_t paylen;
    if (wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) != 0) return;

    if (type == WIRE_DIRECTORY_QUERY && paylen >= sizeof(wire_directory_query_t)) {
        wire_directory_query_t req;
        memcpy(&req, buf, sizeof(req));

        wire_directory_resp_t resp = {.page_id = req.page_id};
        pagedir_value_t v;
        resp.found = raftkv_get(kv, req.page_id, &v) ? 1 : 0;
        if (resp.found) {
            resp.owner_node_id = v.owner_node_id;
            resp.tier = v.tier;
        }
        wire_send_frame_blocking(fd, WIRE_DIRECTORY_RESP, WIRE_FLAG_FINAL, stream_id, &resp, sizeof(resp));
    } else if (type == WIRE_DIRECTORY_UPDATE && paylen >= sizeof(wire_directory_resp_t)) {
        wire_directory_resp_t req;
        memcpy(&req, buf, sizeof(req));

        pagedir_value_t v = {.owner_node_id = req.owner_node_id, .tier = req.tier};
        int rc = raftkv_propose_set(kv, req.page_id, &v);

        wire_directory_resp_t resp = {.page_id = req.page_id};
        if (rc == 0) {
            resp.found = 1;
            resp.owner_node_id = req.owner_node_id;
            resp.tier = req.tier;
        } else {
            resp.found = 0;
            resp.owner_node_id = (uint32_t)raftkv_leader_node_id(kv); /* redirect hint, see header comment */
        }
        wire_send_frame_blocking(fd, WIRE_DIRECTORY_RESP, WIRE_FLAG_FINAL, stream_id, &resp, sizeof(resp));
    } else {
        clog_warn("page-directoryd: unexpected frame type %u", type);
    }
}

int main(int argc, char **argv) {
    clog_init("page-directoryd");
    const char *config_path = "/etc/continuum/page-directoryd.conf";
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
        .log_tag = "page-directoryd/raft",
    };
    raftkv_t *kv = raftkv_start(&raft_cfg, 65536, sizeof(pagedir_value_t));

    int listen_fd = sock_tcp_listen(NULL, dcfg.client_listen_port, 32);
    if (listen_fd < 0) {
        clog_error("page-directoryd: cannot listen on :%u: %s", dcfg.client_listen_port, strerror(errno));
        return 1;
    }
    int fl = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, fl & ~O_NONBLOCK);

    clog_info("page-directoryd: node %u ready, %zu peer(s) in the Raft group, client port :%u",
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
