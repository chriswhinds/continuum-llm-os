#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "pageclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clog.h"
#include "dram_pool.h"
#include "shm_pagetable.h"
#include "sock_util.h"
#include "wire.h"

struct pageclient {
    uint32_t node_id;
    uint64_t page_size;
    char swapd_socket[128];
    shm_pagetable_t *table;
    dram_pool_t pool;
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

pageclient_t *pageclient_connect(uint32_t node_id, uint64_t page_size, const char *swapd_unix_socket) {
    char pt_name[64], pool_name[64];
    snprintf(pt_name, sizeof(pt_name), "/continuum-pt-%u", node_id);
    snprintf(pool_name, sizeof(pool_name), "/continuum-pool-%u", node_id);

    shm_pagetable_t *table = NULL;
    dram_pool_t pool;
    int attached = 0;
    for (int attempt = 0; attempt < 20; attempt++) {
        table = shm_pagetable_open(pt_name, 0, 0);
        if (table && dram_pool_open(&pool, pool_name, 0, page_size, 0) == 0) {
            attached = 1;
            break;
        }
        if (table) {
            shm_pagetable_close(table, pt_name, 0);
            table = NULL;
        }
        clog_warn("pageclient: swapd's shared state (%s) not ready yet, retrying...", pt_name);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 250 * 1000000L};
        nanosleep(&ts, NULL);
    }
    if (!attached) return NULL;

    pageclient_t *pc = calloc(1, sizeof(*pc));
    pc->node_id = node_id;
    pc->page_size = page_size;
    strncpy(pc->swapd_socket, swapd_unix_socket, sizeof(pc->swapd_socket) - 1);
    pc->table = table;
    pc->pool = pool;
    return pc;
}

void pageclient_disconnect(pageclient_t *pc) {
    if (!pc) return;
    dram_pool_close(&pc->pool, NULL, 0);
    shm_pagetable_close(pc->table, NULL, 0);
    free(pc);
}

static int ask_swapd(pageclient_t *pc, uint64_t page_id) {
    int fd = sock_unix_connect(pc->swapd_socket);
    if (fd < 0) {
        clog_error("pageclient: cannot reach swapd at %s", pc->swapd_socket);
        return -1;
    }

    wire_page_fetch_req_t req = {.page_id = page_id};
    if (wire_send_frame_blocking(fd, WIRE_PAGE_FETCH_REQ, 0, 0, &req, sizeof(req)) != 0) {
        close(fd);
        return -1;
    }

    uint8_t type, flags;
    uint64_t stream_id;
    uint32_t paylen;
    size_t bufcap = sizeof(wire_page_fetch_resp_t) + pc->page_size;
    uint8_t *buf = malloc(bufcap);
    int rc = wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, (uint32_t)bufcap, &paylen);
    close(fd);

    int ok = 0;
    if (rc == 0 && type == WIRE_PAGE_FETCH_RESP && paylen >= sizeof(wire_page_fetch_resp_t)) {
        wire_page_fetch_resp_t resp;
        memcpy(&resp, buf, sizeof(resp));
        ok = resp.byte_len == pc->page_size;
    }
    free(buf);
    return ok ? 0 : -1;
}

const void *pageclient_get_page(pageclient_t *pc, uint64_t page_index) {
    uint64_t page_id = ((uint64_t)pc->node_id << 32) | (page_index & 0xFFFFFFFFu);

    shm_page_entry_t e;
    int64_t slot = -1;
    if (shm_pagetable_get(pc->table, page_id, &e, &slot)) {
        return dram_pool_slot(&pc->pool, slot);
    }

    if (ask_swapd(pc, page_id) != 0) return NULL;

    if (shm_pagetable_get(pc->table, page_id, &e, &slot)) {
        return dram_pool_slot(&pc->pool, slot);
    }
    return NULL;
}

uint64_t pageclient_page_size(const pageclient_t *pc) { return pc->page_size; }
