/* Same 3-node-cluster shape as raftnet_test, but through the raftkv
 * abstraction page-directoryd/membershipd will actually use: propose a
 * SET on the leader, confirm it becomes readable (eventually) on every
 * node including followers, then a DEL, then confirm a non-leader
 * refuses writes.
 */
#include "raftkv.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint32_t owner_node_id;
    uint8_t tier;
} test_value_t;

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int main(void) {
    raftnet_peer_t peers[3] = {
        {.node_id = 1, .host = "127.0.0.1", .port = 17811},
        {.node_id = 2, .host = "127.0.0.1", .port = 17812},
        {.node_id = 3, .host = "127.0.0.1", .port = 17813},
    };

    raftkv_t *kvs[3];
    for (int i = 0; i < 3; i++) {
        raftnet_config_t cfg = {
            .my_node_id = peers[i].node_id,
            .peers = peers,
            .n_peers = 3,
            .election_timeout_ms = 300,
            .request_timeout_ms = 80,
            .log_tag = "raftkv_test",
        };
        kvs[i] = raftkv_start(&cfg, 64, sizeof(test_value_t));
        assert(kvs[i]);
    }

    int leader_idx = -1;
    for (int attempt = 0; attempt < 50 && leader_idx < 0; attempt++) {
        sleep_ms(100);
        for (int i = 0; i < 3; i++) {
            if (raftkv_is_leader(kvs[i])) leader_idx = i;
        }
    }
    assert(leader_idx >= 0);
    printf("ok: leader elected\n");

    test_value_t v = {.owner_node_id = 42, .tier = 3};
    assert(raftkv_propose_set(kvs[leader_idx], 1001, &v) == 0);

    int non_leader = (leader_idx + 1) % 3;
    test_value_t junk = {.owner_node_id = 0, .tier = 0};
    assert(raftkv_propose_set(kvs[non_leader], 2002, &junk) == -1); /* not leader */

    int all_have_it = 0;
    for (int attempt = 0; attempt < 30 && !all_have_it; attempt++) {
        sleep_ms(100);
        all_have_it = 1;
        for (int i = 0; i < 3; i++) {
            test_value_t out;
            if (!raftkv_get(kvs[i], 1001, &out) || out.owner_node_id != 42 || out.tier != 3) {
                all_have_it = 0;
            }
        }
    }
    assert(all_have_it);
    printf("ok: a write from the leader replicates to every follower's local state\n");

    assert(raftkv_propose_del(kvs[leader_idx], 1001) == 0);
    int all_deleted = 0;
    for (int attempt = 0; attempt < 30 && !all_deleted; attempt++) {
        sleep_ms(100);
        all_deleted = 1;
        for (int i = 0; i < 3; i++) {
            test_value_t out;
            if (raftkv_get(kvs[i], 1001, &out)) all_deleted = 0;
        }
    }
    assert(all_deleted);
    printf("ok: a delete replicates too\n");

    for (int i = 0; i < 3; i++) raftkv_stop(kvs[i]);
    printf("all raftkv tests passed\n");
    return 0;
}
