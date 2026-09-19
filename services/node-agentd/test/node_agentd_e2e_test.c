/* End-to-end test against the REAL node-agentd binary (not just its
 * source functions): plays swapd's role by creating the shared page
 * table/pool directly, publishes one page, launches node-agentd pointed
 * at the same node_id, and confirms a remote peer's PAGE_FETCH_REQ over
 * real TCP gets the right bytes back -- and that a request for a page
 * that was never published cleanly reports "not found" rather than
 * hanging or crashing.
 */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dram_pool.h"
#include "shm_pagetable.h"
#include "sock_util.h"
#include "wire.h"

#define NODE_ID 42
#define PAGE_SIZE 4096
#define LISTEN_PORT 17901

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int main(void) {
    char pt_name[64], pool_name[64];
    snprintf(pt_name, sizeof(pt_name), "/continuum-pt-%u", NODE_ID);
    snprintf(pool_name, sizeof(pool_name), "/continuum-pool-%u", NODE_ID);

    shm_pagetable_t *table = shm_pagetable_open(pt_name, 16, 1);
    assert(table);
    dram_pool_t pool;
    assert(dram_pool_open(&pool, pool_name, shm_pagetable_capacity(table), PAGE_SIZE, 1) == 0);

    uint64_t published_page_id = ((uint64_t)NODE_ID << 32) | 5;
    int64_t slot = shm_pagetable_put(table, published_page_id, /*tier=*/1, /*now_ns=*/12345);
    assert(slot >= 0);
    uint8_t *slot_bytes = dram_pool_slot(&pool, slot);
    memset(slot_bytes, 0xAB, PAGE_SIZE);

    const char *conf_path = "/tmp/node_agentd_e2e.conf";
    FILE *f = fopen(conf_path, "w");
    fprintf(f, "node_id %u\n", NODE_ID);
    fprintf(f, "listen_port %d\n", LISTEN_PORT);
    fprintf(f, "page_size %d\n", PAGE_SIZE);
    fprintf(f, "health_interval_ms 60000\n"); /* long enough it won't fire during the test */
    fclose(f);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl(NODE_AGENTD_BIN, "node-agentd", "--config", conf_path, (char *)NULL);
        _exit(127);
    }

    /* Give node-agentd a moment to attach to the shared state and start
     * listening, retrying the connect rather than a single fixed sleep. */
    int fd = -1;
    for (int attempt = 0; attempt < 40 && fd < 0; attempt++) {
        sleep_ms(100);
        fd = sock_tcp_connect("127.0.0.1", LISTEN_PORT);
    }
    assert(fd >= 0);
    printf("ok: node-agentd started and accepted a connection\n");

    /* Hit: the page we published above. */
    wire_page_fetch_req_t req = {.page_id = published_page_id};
    assert(wire_send_frame_blocking(fd, WIRE_PAGE_FETCH_REQ, 0, 1, &req, sizeof(req)) == 0);

    uint8_t type, flags;
    uint64_t stream_id;
    uint32_t paylen;
    uint8_t buf[sizeof(wire_page_fetch_resp_t) + PAGE_SIZE];
    assert(wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) == 0);
    assert(type == WIRE_PAGE_FETCH_RESP);
    assert(stream_id == 1);

    wire_page_fetch_resp_t resp;
    memcpy(&resp, buf, sizeof(resp));
    assert(resp.byte_len == PAGE_SIZE);
    for (int i = 0; i < PAGE_SIZE; i++) assert(buf[sizeof(resp) + i] == 0xAB);
    printf("ok: remote PAGE_FETCH_REQ for a resident page returns correct bytes over real TCP\n");

    /* Miss: a page id nobody ever published. */
    wire_page_fetch_req_t req2 = {.page_id = ((uint64_t)NODE_ID << 32) | 999};
    assert(wire_send_frame_blocking(fd, WIRE_PAGE_FETCH_REQ, 0, 2, &req2, sizeof(req2)) == 0);
    assert(wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) == 0);
    memcpy(&resp, buf, sizeof(resp));
    assert(resp.byte_len == 0);
    printf("ok: remote PAGE_FETCH_REQ for a never-published page cleanly reports not-found\n");

    close(fd);
    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);

    dram_pool_close(&pool, pool_name, 1);
    shm_pagetable_close(table, pt_name, 1);
    unlink(conf_path);

    printf("all node-agentd e2e tests passed\n");
    return 0;
}
