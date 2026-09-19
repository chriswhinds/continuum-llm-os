/* swapd -- Continuum's swap manager (ARCH-002 §05): owns this node's
 * tensor address space, demand-pages it from local NVMe, and publishes
 * resident pages into the shared dram_pool/shm_pagetable that node-agentd
 * and shard-execd both read directly. See paging_engine.h for the paging
 * mechanism and its documented fallback when userfaultfd isn't available.
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
#include "dram_pool.h"
#include "paging_engine.h"
#include "shm_pagetable.h"
#include "sock_util.h"
#include "swapd_config.h"
#include "wire.h"

typedef struct {
    paging_engine_t *pe;
    shm_pagetable_t *table;
    dram_pool_t *pool;
    swapd_config_t cfg;
} server_ctx_t;

typedef struct {
    server_ctx_t *ctx;
    wire_conn_t *conn;
} dispatch_ctx_t;

static void *lookup_slot_bytes(server_ctx_t *ctx, uint64_t page_id) {
    shm_page_entry_t e;
    int64_t slot = -1;
    if (!shm_pagetable_get(ctx->table, page_id, &e, &slot)) return NULL;
    return dram_pool_slot(ctx->pool, slot);
}

static void dispatch(void *user, uint8_t type, uint8_t flags, uint64_t stream_id,
                      const uint8_t *payload, uint32_t paylen) {
    (void)flags;
    dispatch_ctx_t *d = user;
    server_ctx_t *ctx = d->ctx;

    if (type != WIRE_PAGE_FETCH_REQ || paylen < sizeof(wire_page_fetch_req_t)) return;
    wire_page_fetch_req_t req;
    memcpy(&req, payload, sizeof(req));

    uint32_t req_node = (uint32_t)(req.page_id >> 32);
    uint64_t page_index = req.page_id & 0xFFFFFFFFu;

    int ok = 0;
    if (req_node == ctx->cfg.node_id) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        ok = (paging_engine_ensure_resident(ctx->pe, page_index, now_ns) == 0);
    } else {
        clog_warn("swapd: PAGE_FETCH_REQ for page_id=0x%llx belongs to node %u, not us (%u)",
                  (unsigned long long)req.page_id, req_node, ctx->cfg.node_id);
    }

    wire_page_fetch_resp_t resp = {0};
    resp.page_id = req.page_id;
    resp.tier = 1; /* WIRE_TIER_LOCAL_DRAM -- ensure_resident() just published it there */
    resp.byte_len = ok ? (uint32_t)ctx->cfg.page_size : 0;

    if (!ok) {
        wire_conn_enqueue(d->conn, WIRE_PAGE_FETCH_RESP, 0, stream_id, &resp, sizeof(resp));
        wire_conn_flush(d->conn);
        return;
    }

    size_t total = sizeof(resp) + ctx->cfg.page_size;
    uint8_t *out = malloc(total);
    memcpy(out, &resp, sizeof(resp));
    void *bytes = lookup_slot_bytes(ctx, req.page_id);
    if (bytes) memcpy(out + sizeof(resp), bytes, ctx->cfg.page_size);
    else memset(out + sizeof(resp), 0, ctx->cfg.page_size);

    wire_conn_enqueue(d->conn, WIRE_PAGE_FETCH_RESP, 0, stream_id, out, (uint32_t)total);
    wire_conn_flush(d->conn);
    free(out);
}

int main(int argc, char **argv) {
    clog_init("swapd");
    const char *config_path = "/etc/continuum/swapd.conf";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) clog_set_level(CLOG_DEBUG);
    }

    swapd_config_t cfg;
    if (swapd_config_load(config_path, &cfg) != 0) return 1;

    char pt_name[64], pool_name[64];
    snprintf(pt_name, sizeof(pt_name), "/continuum-pt-%u", cfg.node_id);
    snprintf(pool_name, sizeof(pool_name), "/continuum-pool-%u", cfg.node_id);

    shm_pagetable_t *table = shm_pagetable_open(pt_name, cfg.dram_capacity_pages, /*is_creator=*/1);
    if (!table) {
        clog_error("swapd: failed to create shared page table %s", pt_name);
        return 1;
    }
    dram_pool_t pool;
    if (dram_pool_open(&pool, pool_name, shm_pagetable_capacity(table), cfg.page_size, /*is_creator=*/1) != 0) {
        clog_error("swapd: failed to create shared dram pool %s", pool_name);
        return 1;
    }

    paging_engine_t *pe = paging_engine_create(&cfg, table, &pool);
    if (!pe) return 1;

    clog_info("swapd: node %u ready -- %llu-byte tensor region, %llu-page DRAM budget, uffd=%s",
              cfg.node_id, (unsigned long long)cfg.tensor_region_bytes,
              (unsigned long long)shm_pagetable_capacity(table),
              paging_engine_using_real_uffd(pe) ? "real" : "simulated (see log above)");

    int listen_fd = sock_unix_listen(cfg.unix_socket, 16);
    if (listen_fd < 0) {
        clog_error("swapd: cannot listen on %s: %s", cfg.unix_socket, strerror(errno));
        return 1;
    }

    /* Sentinel addresses distinguish the listen socket and the tick timer
     * from a connection's wire_conn_t* in epoll_event.data.ptr -- the same
     * pattern lib/raftnet uses, and safer than mixing .data.fd/.data.ptr
     * reads on the same union. */
    int listen_marker, timer_marker;

    int epfd = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = &listen_marker};
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    /* TFD_NONBLOCK matters here, not just as a style choice: the drain
     * loop below (`while read()==8`) relies on a second read() returning
     * EAGAIN once fully drained. On a blocking fd, that second read()
     * instead blocks until the *next* tick, which loops and blocks again
     * forever -- the eviction sweep below would never actually run. */
    int tickfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec its = {.it_interval = {.tv_sec = 1, .tv_nsec = 0}, .it_value = {.tv_sec = 1, .tv_nsec = 0}};
    timerfd_settime(tickfd, 0, &its, NULL);
    ev.data.ptr = &timer_marker;
    epoll_ctl(epfd, EPOLL_CTL_ADD, tickfd, &ev);

    server_ctx_t ctx = {.pe = pe, .table = table, .pool = &pool, .cfg = cfg};

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
                size_t resident = paging_engine_resident_count(pe);
                size_t cap = paging_engine_capacity_pages(pe);
                if (cap > 0 && resident * 100 >= (size_t)cfg.high_watermark_pct * cap) {
                    paging_engine_evict_lru(pe, cap / 10 + 1);
                }
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
