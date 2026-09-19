/* node-agentd -- ARCH-002 §05: owns this node's DRAM-pool bookkeeping (via
 * the shared page table/pool swapd created), answers remote PAGE_FETCH_REQ
 * for pages resident in this node's DRAM, and pushes health telemetry.
 *
 * Scope note (see also ARCH-002's "§05 Node Agent" and paging_engine.h):
 * a remote fetch that misses locally is reported found=0 rather than
 * triggering a page-in from this node's local NVMe on the requester's
 * behalf. Every page this node ever resolves via its own swapd is
 * reachable here the moment it's published to the shared table -- this
 * only affects a peer asking for a page that has never been touched
 * locally at all, or one that swapd has since evicted.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "clog.h"
#include "cpu_load.h"
#include "dram_pool.h"
#include "node_agent_config.h"
#include "shm_pagetable.h"
#include "sock_util.h"
#include "wire.h"

typedef struct {
    node_agent_config_t cfg;
    shm_pagetable_t *table;
    dram_pool_t *pool;
} server_ctx_t;

typedef struct {
    server_ctx_t *ctx;
    wire_conn_t *conn;
} dispatch_ctx_t;

/* Retries attaching to swapd's shared segments for a few seconds, since
 * continuumd may have started node-agentd before swapd has finished
 * creating them (see ARCH-002 §06's service ordering note). */
static int attach_shared_state(uint32_t node_id, uint64_t page_size, shm_pagetable_t **out_table, dram_pool_t *out_pool) {
    char pt_name[64], pool_name[64];
    snprintf(pt_name, sizeof(pt_name), "/continuum-pt-%u", node_id);
    snprintf(pool_name, sizeof(pool_name), "/continuum-pool-%u", node_id);

    for (int attempt = 0; attempt < 20; attempt++) {
        *out_table = shm_pagetable_open(pt_name, 0, /*is_creator=*/0);
        if (*out_table && dram_pool_open(out_pool, pool_name, 0, page_size, /*is_creator=*/0) == 0) {
            return 0;
        }
        if (*out_table) {
            shm_pagetable_close(*out_table, pt_name, 0);
            *out_table = NULL;
        }
        clog_warn("node-agentd: swapd's shared state (%s) not ready yet, retrying...", pt_name);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 250 * 1000000L};
        nanosleep(&ts, NULL);
    }
    return -1;
}

static void dispatch(void *user, uint8_t type, uint8_t flags, uint64_t stream_id,
                      const uint8_t *payload, uint32_t paylen) {
    (void)flags;
    dispatch_ctx_t *d = user;
    server_ctx_t *ctx = d->ctx;

    if (type != WIRE_PAGE_FETCH_REQ || paylen < sizeof(wire_page_fetch_req_t)) return;
    wire_page_fetch_req_t req;
    memcpy(&req, payload, sizeof(req));

    shm_page_entry_t e;
    int64_t slot = -1;
    int found = shm_pagetable_get(ctx->table, req.page_id, &e, &slot);

    wire_page_fetch_resp_t resp = {0};
    resp.page_id = req.page_id;
    resp.tier = found ? e.tier : 0;
    resp.byte_len = found ? (uint32_t)ctx->cfg.page_size : 0;

    if (!found) {
        clog_debug("node-agentd: PAGE_FETCH_REQ miss for page_id=0x%llx", (unsigned long long)req.page_id);
        wire_conn_enqueue(d->conn, WIRE_PAGE_FETCH_RESP, 0, stream_id, &resp, sizeof(resp));
        wire_conn_flush(d->conn);
        return;
    }

    size_t total = sizeof(resp) + ctx->cfg.page_size;
    uint8_t *out = malloc(total);
    memcpy(out, &resp, sizeof(resp));
    void *bytes = dram_pool_slot(ctx->pool, slot);
    if (bytes) memcpy(out + sizeof(resp), bytes, ctx->cfg.page_size);
    else memset(out + sizeof(resp), 0, ctx->cfg.page_size);

    wire_conn_enqueue(d->conn, WIRE_PAGE_FETCH_RESP, 0, stream_id, out, (uint32_t)total);
    wire_conn_flush(d->conn);
    free(out);
}

static void send_health_report(server_ctx_t *ctx) {
    wire_health_report_t report = {0};
    report.node_id = ctx->cfg.node_id;
    report.cpu_load_pct = cpu_load_sample();
    report.dram_used_bytes = (uint64_t)shm_pagetable_count(ctx->table) * ctx->cfg.page_size;
    report.dram_total_bytes = (uint64_t)shm_pagetable_capacity(ctx->table) * ctx->cfg.page_size;
    report.swap_io_bytes_per_sec = 0; /* see module comment: not wired to swapd in this reference build */

    for (size_t i = 0; i < ctx->cfg.n_sinks; i++) {
        int fd = sock_tcp_connect(ctx->cfg.sinks[i].host, ctx->cfg.sinks[i].port);
        if (fd < 0) {
            clog_warn("node-agentd: cannot reach telemetry sink %s:%u", ctx->cfg.sinks[i].host, ctx->cfg.sinks[i].port);
            continue;
        }
        wire_send_frame_blocking(fd, WIRE_HEALTH_REPORT, WIRE_FLAG_FINAL, 0, &report, sizeof(report));
        close(fd);
    }
}

int main(int argc, char **argv) {
    clog_init("node-agentd");
    const char *config_path = "/etc/continuum/node-agentd.conf";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) clog_set_level(CLOG_DEBUG);
    }

    node_agent_config_t cfg;
    if (node_agent_config_load(config_path, &cfg) != 0) return 1;

    shm_pagetable_t *table = NULL;
    dram_pool_t pool;
    if (attach_shared_state(cfg.node_id, cfg.page_size, &table, &pool) != 0) {
        clog_error("node-agentd: could not attach to swapd's shared state -- is swapd running for node %u?", cfg.node_id);
        return 1;
    }
    clog_info("node-agentd: attached to swapd's shared state -- %zu pages capacity",
              shm_pagetable_capacity(table));

    int listen_fd = sock_tcp_listen(NULL, cfg.listen_port, 32);
    if (listen_fd < 0) {
        clog_error("node-agentd: cannot listen on port %u: %s", cfg.listen_port, strerror(errno));
        return 1;
    }
    clog_info("node-agentd: node %u listening on :%u for remote page fetches", cfg.node_id, cfg.listen_port);

    int epfd = epoll_create1(0);
    int listen_marker, timer_marker;
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = &listen_marker};
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    /* TFD_NONBLOCK matters here, not just as a style choice: the drain
     * loop below (`while read()==8`) relies on a second read() returning
     * EAGAIN once fully drained. On a blocking fd, that second read()
     * instead blocks until the *next* tick fires -- which then loops
     * again and blocks on the tick after that, forever, so the health
     * report is never actually sent. Caught by console_e2e_test finding
     * no telemetry ever arrived. */
    int tickfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    long ms = cfg.health_interval_ms;
    struct itimerspec its = {
        .it_interval = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L},
        .it_value = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L},
    };
    timerfd_settime(tickfd, 0, &its, NULL);
    ev.data.ptr = &timer_marker;
    epoll_ctl(epfd, EPOLL_CTL_ADD, tickfd, &ev);

    server_ctx_t ctx = {.cfg = cfg, .table = table, .pool = &pool};

    struct epoll_event events[16];
    for (;;) {
        int n = epoll_wait(epfd, events, 16, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            void *ptr = events[i].data.ptr;
            if (ptr == &listen_marker) {
                for (;;) {
                    int cfd = accept(listen_fd, NULL, NULL);
                    if (cfd < 0) break;
                    sock_set_nonblocking(cfd);
                    wire_conn_t *conn = wire_conn_new(cfd);
                    struct epoll_event cev = {.events = EPOLLIN, .data.ptr = conn};
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
                continue;
            }
            if (ptr == &timer_marker) {
                uint64_t exp;
                while (read(tickfd, &exp, sizeof(exp)) == sizeof(exp)) {}
                send_health_report(&ctx);
                continue;
            }
            wire_conn_t *conn = ptr;
            dispatch_ctx_t d = {.ctx = &ctx, .conn = conn};
            if (wire_conn_on_readable(conn, dispatch, &d) != 0) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, wire_conn_fd(conn), NULL);
                close(wire_conn_fd(conn));
                wire_conn_free(conn);
            }
        }
    }
    return 0;
}
