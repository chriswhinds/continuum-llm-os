/* Real 3-node page-directoryd cluster: writes a directory entry (retrying
 * across nodes until one accepts as leader), then confirms it becomes
 * readable via WIRE_DIRECTORY_QUERY on every node -- including the ones
 * that never accepted the write -- proving actual Raft replication
 * through the real client TCP protocol, not just the raftkv library.
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
static const int RAFT_PORTS[N_NODES] = {17821, 17822, 17823};
static const int CLIENT_PORTS[N_NODES] = {17831, 17832, 17833};

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int main(void) {
    char conf_paths[N_NODES][64];
    pid_t pids[N_NODES];

    for (int i = 0; i < N_NODES; i++) {
        snprintf(conf_paths[i], sizeof(conf_paths[i]), "/tmp/pagedir_e2e_%d.conf", i);
        FILE *f = fopen(conf_paths[i], "w");
        fprintf(f, "node_id %d\n", i + 1);
        for (int j = 0; j < N_NODES; j++) {
            fprintf(f, "peer %d 127.0.0.1 %d\n", j + 1, RAFT_PORTS[j]);
        }
        fprintf(f, "client_listen_port %d\n", CLIENT_PORTS[i]);
        fclose(f);
    }

    for (int i = 0; i < N_NODES; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            execl(PAGE_DIRECTORYD_BIN, "page-directoryd", "--config", conf_paths[i], (char *)NULL);
            _exit(127);
        }
    }
    sleep_ms(300);

    uint64_t page_id = 0x0000000700000042ULL; /* node 7's page 0x42, arbitrary */
    wire_directory_resp_t write_req = {.page_id = page_id, .owner_node_id = 7, .tier = 1};

    /* page-directoryd runs a production-realistic 1000ms election timeout
     * (unlike raftnet_test/raftkv_test's fast 300ms, tuned for speed) --
     * the first election doesn't even start until ~1-2s in, so this needs
     * real patience, especially under a loaded CI/sandbox machine. */
    int wrote = 0;
    for (int attempt = 0; attempt < 60 && !wrote; attempt++) {
        for (int i = 0; i < N_NODES && !wrote; i++) {
            int fd = sock_tcp_connect("127.0.0.1", CLIENT_PORTS[i]);
            if (fd < 0) continue;
            if (wire_send_frame_blocking(fd, WIRE_DIRECTORY_UPDATE, 0, 1, &write_req, sizeof(write_req)) == 0) {
                uint8_t type, flags;
                uint64_t stream_id;
                uint8_t buf[64];
                uint32_t paylen;
                if (wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) == 0 &&
                    type == WIRE_DIRECTORY_RESP) {
                    wire_directory_resp_t resp;
                    memcpy(&resp, buf, sizeof(resp));
                    if (resp.found) wrote = 1;
                }
            }
            close(fd);
        }
        if (!wrote) sleep_ms(200);
    }
    assert(wrote);
    printf("ok: directory update accepted by the current leader\n");

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
            wire_directory_query_t q = {.page_id = page_id};
            wire_send_frame_blocking(fd, WIRE_DIRECTORY_QUERY, 0, 2, &q, sizeof(q));
            uint8_t type, flags;
            uint64_t stream_id;
            uint8_t buf[64];
            uint32_t paylen;
            int ok = 0;
            if (wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) == 0 &&
                type == WIRE_DIRECTORY_RESP) {
                wire_directory_resp_t resp;
                memcpy(&resp, buf, sizeof(resp));
                ok = resp.found && resp.owner_node_id == 7 && resp.tier == 1;
            }
            close(fd);
            if (!ok) all_replicated = 0;
        }
    }
    assert(all_replicated);
    printf("ok: the entry is readable via WIRE_DIRECTORY_QUERY on all 3 nodes -- real Raft replication over the real client protocol\n");

    for (int i = 0; i < N_NODES; i++) kill(pids[i], SIGTERM);
    for (int i = 0; i < N_NODES; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        unlink(conf_paths[i]);
    }

    printf("all page-directoryd e2e tests passed\n");
    return 0;
}
