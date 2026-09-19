/* Same shape as page-directoryd's e2e test, against a real 3-node
 * membershipd cluster and the WIRE_MEMBERSHIP_* client protocol.
 */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sock_util.h"
#include "wire.h"

#define N_NODES 3
static const int RAFT_PORTS[N_NODES] = {17841, 17842, 17843};
static const int CLIENT_PORTS[N_NODES] = {17851, 17852, 17853};

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int main(void) {
    char conf_paths[N_NODES][64];
    pid_t pids[N_NODES];

    for (int i = 0; i < N_NODES; i++) {
        snprintf(conf_paths[i], sizeof(conf_paths[i]), "/tmp/membership_e2e_%d.conf", i);
        FILE *f = fopen(conf_paths[i], "w");
        fprintf(f, "node_id %d\n", i + 1);
        for (int j = 0; j < N_NODES; j++) fprintf(f, "peer %d 127.0.0.1 %d\n", j + 1, RAFT_PORTS[j]);
        fprintf(f, "client_listen_port %d\n", CLIENT_PORTS[i]);
        fclose(f);
    }

    for (int i = 0; i < N_NODES; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            execl(MEMBERSHIPD_BIN, "membershipd", "--config", conf_paths[i], (char *)NULL);
            _exit(127);
        }
    }
    sleep_ms(300);

    uint32_t target_node = 9;
    wire_membership_resp_t write_req = {.node_id = target_node, .status = WIRE_NODE_UP, .last_heartbeat_ns = 123456789};

    /* See pagedir_e2e_test.c's comment: production-realistic 1000ms
     * election timeout needs real patience here, not the fast 300ms used
     * by raftnet_test/raftkv_test. */
    int wrote = 0;
    for (int attempt = 0; attempt < 60 && !wrote; attempt++) {
        for (int i = 0; i < N_NODES && !wrote; i++) {
            int fd = sock_tcp_connect("127.0.0.1", CLIENT_PORTS[i]);
            if (fd < 0) continue;
            if (wire_send_frame_blocking(fd, WIRE_MEMBERSHIP_UPDATE, 0, 1, &write_req, sizeof(write_req)) == 0) {
                uint8_t type, flags;
                uint64_t stream_id;
                uint8_t buf[64];
                uint32_t paylen;
                if (wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) == 0 &&
                    type == WIRE_MEMBERSHIP_RESP) {
                    wire_membership_resp_t resp;
                    memcpy(&resp, buf, sizeof(resp));
                    if (resp.found) wrote = 1;
                }
            }
            close(fd);
        }
        if (!wrote) sleep_ms(200);
    }
    assert(wrote);
    printf("ok: membership update accepted by the current leader\n");

    int all_replicated = 0;
    for (int attempt = 0; attempt < 50 && !all_replicated; attempt++) {
        sleep_ms(100);
        all_replicated = 1;
        for (int i = 0; i < N_NODES; i++) {
            int fd = sock_tcp_connect("127.0.0.1", CLIENT_PORTS[i]);
            if (fd < 0) {
                all_replicated = 0;
                continue;
            }
            wire_membership_query_t q = {.node_id = target_node};
            wire_send_frame_blocking(fd, WIRE_MEMBERSHIP_QUERY, 0, 2, &q, sizeof(q));
            uint8_t type, flags;
            uint64_t stream_id;
            uint8_t buf[64];
            uint32_t paylen;
            int ok = 0;
            if (wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) == 0 &&
                type == WIRE_MEMBERSHIP_RESP) {
                wire_membership_resp_t resp;
                memcpy(&resp, buf, sizeof(resp));
                ok = resp.found && resp.status == WIRE_NODE_UP && resp.last_heartbeat_ns == 123456789;
            }
            close(fd);
            if (!ok) all_replicated = 0;
        }
    }
    assert(all_replicated);
    printf("ok: membership entry replicated and readable on all 3 nodes\n");

    for (int i = 0; i < N_NODES; i++) kill(pids[i], SIGTERM);
    for (int i = 0; i < N_NODES; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        unlink(conf_paths[i]);
    }

    printf("all membershipd e2e tests passed\n");
    return 0;
}
