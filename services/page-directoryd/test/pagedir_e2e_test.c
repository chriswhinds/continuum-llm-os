/* Real 3-node page-directoryd cluster: writes a directory entry (retrying
 * across nodes until one accepts as leader), then confirms it becomes
 * readable via WIRE_DIRECTORY_QUERY on every node -- including the ones
 * that never accepted the write -- proving actual Raft replication
 * through the real client TCP protocol, not just the raftkv library.
 *
 * Ports are derived from this process's own pid rather than hardcoded, so
 * a stuck/orphaned process from a previous failed run (e.g. one killed by
 * ctest's timeout before it could clean up its own children -- assert()
 * calls abort(), which skips the kill()/waitpid() below) can never block
 * this run by squatting on a fixed port; and any failure here goes
 * through cleanup_and_exit() rather than a bare assert(), so THIS run
 * doesn't leave orphans of its own for the next one to trip over.
 */
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sock_util.h"
#include "wire.h"

#define N_NODES 3
static int RAFT_PORTS[N_NODES];
static int CLIENT_PORTS[N_NODES];
static pid_t g_pids[N_NODES];
static char g_conf_paths[N_NODES][64];

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void cleanup_and_exit(int ok, const char *failure_msg) {
    for (int i = 0; i < N_NODES; i++) {
        if (g_pids[i] > 0) kill(g_pids[i], SIGTERM);
    }
    for (int i = 0; i < N_NODES; i++) {
        if (g_pids[i] > 0) {
            int status;
            waitpid(g_pids[i], &status, 0);
        }
        unlink(g_conf_paths[i]);
    }
    if (!ok) {
        fprintf(stderr, "pagedir_e2e_test FAILED: %s\n", failure_msg);
        exit(1);
    }
    printf("all page-directoryd e2e tests passed\n");
    exit(0);
}

int main(void) {
    int port_base = 20000 + (getpid() % 20000);
    for (int i = 0; i < N_NODES; i++) {
        RAFT_PORTS[i] = port_base + i;
        CLIENT_PORTS[i] = port_base + N_NODES + i;
    }

    for (int i = 0; i < N_NODES; i++) {
        snprintf(g_conf_paths[i], sizeof(g_conf_paths[i]), "/tmp/pagedir_e2e_%d_%d.conf", (int)getpid(), i);
        FILE *f = fopen(g_conf_paths[i], "w");
        fprintf(f, "node_id %d\n", i + 1);
        for (int j = 0; j < N_NODES; j++) {
            fprintf(f, "peer %d 127.0.0.1 %d\n", j + 1, RAFT_PORTS[j]);
        }
        fprintf(f, "client_listen_port %d\n", CLIENT_PORTS[i]);
        fclose(f);
    }

    for (int i = 0; i < N_NODES; i++) {
        g_pids[i] = fork();
        if (g_pids[i] == 0) {
            /* Redirect away from whatever stdout/stderr this test
             * inherited (a pipe back to ctest, when run under it) --
             * three daemons writing concurrently into that same pipe is
             * unnecessary contention this test doesn't need to pay for. */
            char log_path[96];
            snprintf(log_path, sizeof(log_path), "/tmp/pagedir_e2e_%d_%d.log", (int)getpid(), i);
            int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }
            execl(PAGE_DIRECTORYD_BIN, "page-directoryd", "--config", g_conf_paths[i], (char *)NULL);
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
    if (!wrote) cleanup_and_exit(0, "no leader accepted the directory update within the retry budget");
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
    if (!all_replicated) cleanup_and_exit(0, "the write did not replicate to all 3 nodes within the retry budget");
    printf("ok: the entry is readable via WIRE_DIRECTORY_QUERY on all 3 nodes -- real Raft replication over the real client protocol\n");

    cleanup_and_exit(1, NULL);
    return 0;
}
