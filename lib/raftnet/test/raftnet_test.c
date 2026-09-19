/* Integration test: brings up a real 3-node raftnet cluster on loopback,
 * waits for a leader to be elected, proposes an entry on the leader, and
 * checks it gets applied on all three nodes. This is the thing that proves
 * the wire (de)serialization and the raft.c callback wiring are actually
 * correct together, not just individually. */
#include "raftnet.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char applied[16][256];
    int n_applied;
} apply_log_t;

static apply_log_t g_logs[3];

static int apply_cb(const void *data, size_t len, void *user) {
    apply_log_t *log = user;
    if (log->n_applied < 16) {
        size_t n = len < 255 ? len : 255;
        memcpy(log->applied[log->n_applied], data, n);
        log->applied[log->n_applied][n] = '\0';
        log->n_applied++;
    }
    return 0;
}

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int main(void) {
    raftnet_peer_t peers[3] = {
        {.node_id = 1, .host = "127.0.0.1", .port = 17801},
        {.node_id = 2, .host = "127.0.0.1", .port = 17802},
        {.node_id = 3, .host = "127.0.0.1", .port = 17803},
    };

    raftnet_t *nodes[3];
    for (int i = 0; i < 3; i++) {
        raftnet_config_t cfg = {
            .my_node_id = peers[i].node_id,
            .peers = peers,
            .n_peers = 3,
            .apply = apply_cb,
            .apply_user = &g_logs[i],
            .election_timeout_ms = 300,
            .request_timeout_ms = 80,
            .log_tag = "raftnet_test",
        };
        nodes[i] = raftnet_start(&cfg);
        assert(nodes[i] != NULL);
    }

    /* Give the cluster time to connect and elect a leader. Election timeout
     * is 300ms; a few seconds of headroom keeps this robust on a loaded CI
     * box without making a passing run slow. */
    int leader_idx = -1;
    for (int attempt = 0; attempt < 50 && leader_idx < 0; attempt++) {
        sleep_ms(100);
        for (int i = 0; i < 3; i++) {
            if (raftnet_is_leader(nodes[i])) {
                leader_idx = i;
                break;
            }
        }
    }
    assert(leader_idx >= 0);
    printf("ok: leader elected (node index %d) within timeout\n", leader_idx);

    /* Every node should agree on who the leader is. */
    int expected_leader_id = peers[leader_idx].node_id;
    for (int attempt = 0; attempt < 20; attempt++) {
        int all_agree = 1;
        for (int i = 0; i < 3; i++) {
            if (raftnet_leader_node_id(nodes[i]) != expected_leader_id) all_agree = 0;
        }
        if (all_agree) break;
        sleep_ms(100);
    }
    for (int i = 0; i < 3; i++) {
        assert(raftnet_leader_node_id(nodes[i]) == expected_leader_id);
    }
    printf("ok: all nodes agree on the leader\n");

    /* Propose from the leader; a non-leader must refuse. */
    int non_leader_idx = (leader_idx + 1) % 3;
    assert(raftnet_propose(nodes[non_leader_idx], "nope", 4) == -1);

    const char *msg = "directory-update:page42->node3";
    assert(raftnet_propose(nodes[leader_idx], msg, strlen(msg)) == 0);

    int applied_everywhere = 0;
    for (int attempt = 0; attempt < 30 && !applied_everywhere; attempt++) {
        sleep_ms(100);
        applied_everywhere = 1;
        for (int i = 0; i < 3; i++) {
            int found = 0;
            for (int j = 0; j < g_logs[i].n_applied; j++) {
                if (strcmp(g_logs[i].applied[j], msg) == 0) found = 1;
            }
            if (!found) applied_everywhere = 0;
        }
    }
    assert(applied_everywhere);
    printf("ok: proposed entry replicated and applied on all 3 nodes\n");

    for (int i = 0; i < 3; i++) raftnet_stop(nodes[i]);

    printf("all raftnet tests passed\n");
    return 0;
}
