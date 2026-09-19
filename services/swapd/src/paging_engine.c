#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "paging_engine.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "clog.h"
#include "sock_util.h"
#include "wire.h"

struct paging_engine {
    swapd_config_t cfg;
    shm_pagetable_t *table;
    dram_pool_t *pool;

    void *region;
    uint64_t n_pages;
    int weight_fd;

    int uffd; /* -1 if not using real userfaultfd (see header comment) */
    pthread_t fault_thread;
    volatile int stop_flag;

    uint8_t *resident_bitmap; /* touched only by the request-handling thread, see header comment */
};

static int bitmap_get(const uint8_t *bm, uint64_t idx) { return (bm[idx / 8] >> (idx % 8)) & 1; }
static void bitmap_set(uint8_t *bm, uint64_t idx) { bm[idx / 8] |= (uint8_t)(1u << (idx % 8)); }
static void bitmap_clear(uint8_t *bm, uint64_t idx) { bm[idx / 8] &= (uint8_t)~(1u << (idx % 8)); }

static uint64_t make_page_id(uint32_t node_id, uint64_t page_index) {
    return ((uint64_t)node_id << 32) | (page_index & 0xFFFFFFFFu);
}

/* Reads page_index's content into buf (page_size bytes). Tries the local
 * weight file first; on a short/failed read, tries each configured remote
 * peer in order (ARCH-001 Fig. 3's tier 2/4 -- see paging_engine.h's
 * module comment on why this reference build folds "remote DRAM" and
 * "remote NVMe" into one uniform PAGE_FETCH_REQ/RESP exchange: from the
 * requester's side, both just mean "ask a specific peer for this page,"
 * regardless of which tier that peer happens to be serving it from).
 * Returns 0 if buf now holds real data, -1 if nothing could supply it (buf
 * is zero-filled in that case so callers can still safely use it). */
static int resolve_page_content(paging_engine_t *pe, uint64_t page_index, uint8_t *buf) {
    size_t page_size = (size_t)pe->cfg.page_size;
    off_t off = (off_t)(page_index * pe->cfg.page_size);

    ssize_t n = pread(pe->weight_fd, buf, page_size, off);
    if (n == (ssize_t)page_size) return 0;

    clog_warn("swapd: local weight file short/failed read for page_index=%llu (got %zd of %zu) -- trying %zu remote peer(s)",
              (unsigned long long)page_index, n, page_size, pe->cfg.n_peers);

    uint64_t page_id = make_page_id(pe->cfg.node_id, page_index);
    for (size_t i = 0; i < pe->cfg.n_peers; i++) {
        int fd = sock_tcp_connect(pe->cfg.peers[i].host, pe->cfg.peers[i].port);
        if (fd < 0) continue;

        wire_page_fetch_req_t req = {.page_id = page_id};
        if (wire_send_frame_blocking(fd, WIRE_PAGE_FETCH_REQ, 0, 0, &req, sizeof(req)) != 0) {
            close(fd);
            continue;
        }
        uint8_t type, flags;
        uint64_t stream_id;
        uint32_t paylen;
        size_t respbuf_cap = sizeof(wire_page_fetch_resp_t) + page_size;
        uint8_t *respbuf = malloc(respbuf_cap);
        int rc = wire_recv_frame_blocking(fd, &type, &flags, &stream_id, respbuf, (uint32_t)respbuf_cap, &paylen);
        close(fd);
        if (rc != 0 || type != WIRE_PAGE_FETCH_RESP || paylen < sizeof(wire_page_fetch_resp_t)) {
            free(respbuf);
            continue;
        }

        wire_page_fetch_resp_t resp;
        memcpy(&resp, respbuf, sizeof(resp));
        if (resp.byte_len != page_size) {
            free(respbuf);
            continue;
        }
        memcpy(buf, respbuf + sizeof(resp), page_size);
        free(respbuf);
        clog_info("swapd: resolved page_index=%llu from remote peer %s:%u", (unsigned long long)page_index,
                   pe->cfg.peers[i].host, pe->cfg.peers[i].port);
        return 0;
    }

    clog_error("swapd: page_index=%llu unresolvable locally or remotely -- serving zeros", (unsigned long long)page_index);
    memset(buf, 0, page_size);
    return -1;
}

static void *fault_thread_main(void *arg) {
    paging_engine_t *pe = arg;
    size_t page_size = (size_t)pe->cfg.page_size;
    uint8_t *buf = malloc(page_size);

    struct pollfd pfd = {.fd = pe->uffd, .events = POLLIN};
    while (!pe->stop_flag) {
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) continue;

        struct uffd_msg msg;
        ssize_t n = read(pe->uffd, &msg, sizeof(msg));
        if (n != (ssize_t)sizeof(msg)) continue;
        if (msg.event != UFFD_EVENT_PAGEFAULT) continue;

        uintptr_t fault_addr = (uintptr_t)msg.arg.pagefault.address;
        uintptr_t region_base = (uintptr_t)pe->region;
        uint64_t page_index = (fault_addr - region_base) / pe->cfg.page_size;
        uintptr_t page_aligned = region_base + page_index * pe->cfg.page_size;

        resolve_page_content(pe, page_index, buf); /* zero-fills buf on failure, never leaves it undefined */

        struct uffdio_copy copy = {
            .dst = page_aligned,
            .src = (uint64_t)(uintptr_t)buf,
            .len = pe->cfg.page_size,
            .mode = 0,
        };
        if (ioctl(pe->uffd, UFFDIO_COPY, &copy) != 0) {
            clog_error("swapd: UFFDIO_COPY failed for page_index=%llu: %s", (unsigned long long)page_index, strerror(errno));
        }
    }
    free(buf);
    return NULL;
}

static int try_setup_real_uffd(paging_engine_t *pe) {
    long fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        clog_warn("swapd: userfaultfd() unavailable (%s) -- falling back to explicit page-in "
                  "simulation. On the target Pi 5 image, or as root, or after "
                  "'sysctl -w vm.unprivileged_userfaultfd=1', this uses the real kernel "
                  "mechanism instead.", strerror(errno));
        return -1;
    }

    struct uffdio_api api = {.api = UFFD_API, .features = 0};
    if (ioctl((int)fd, UFFDIO_API, &api) != 0) {
        clog_warn("swapd: UFFDIO_API failed (%s), falling back", strerror(errno));
        close((int)fd);
        return -1;
    }

    struct uffdio_register reg = {
        .range = {.start = (uint64_t)(uintptr_t)pe->region, .len = pe->cfg.tensor_region_bytes},
        .mode = UFFDIO_REGISTER_MODE_MISSING,
    };
    if (ioctl((int)fd, UFFDIO_REGISTER, &reg) != 0) {
        clog_warn("swapd: UFFDIO_REGISTER failed (%s), falling back", strerror(errno));
        close((int)fd);
        return -1;
    }

    clog_info("swapd: real userfaultfd demand paging active on %llu bytes",
              (unsigned long long)pe->cfg.tensor_region_bytes);
    return (int)fd;
}

paging_engine_t *paging_engine_create(const swapd_config_t *cfg, shm_pagetable_t *table, dram_pool_t *pool) {
    paging_engine_t *pe = calloc(1, sizeof(*pe));
    pe->cfg = *cfg;
    pe->table = table;
    pe->pool = pool;
    pe->n_pages = cfg->tensor_region_bytes / cfg->page_size;

    pe->weight_fd = open(cfg->weight_file, O_RDONLY);
    if (pe->weight_fd < 0) {
        clog_error("swapd: cannot open weight file %s: %s", cfg->weight_file, strerror(errno));
    }

    pe->region = mmap(NULL, cfg->tensor_region_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pe->region == MAP_FAILED) {
        clog_error("swapd: mmap of %llu-byte tensor region failed: %s",
                   (unsigned long long)cfg->tensor_region_bytes, strerror(errno));
        free(pe);
        return NULL;
    }

    pe->resident_bitmap = calloc((pe->n_pages + 7) / 8, 1);

    pe->uffd = try_setup_real_uffd(pe);
    if (pe->uffd >= 0) {
        pthread_create(&pe->fault_thread, NULL, fault_thread_main, pe);
    }

    return pe;
}

void paging_engine_destroy(paging_engine_t *pe) {
    if (!pe) return;
    if (pe->uffd >= 0) {
        pe->stop_flag = 1;
        pthread_join(pe->fault_thread, NULL);
        close(pe->uffd);
    }
    if (pe->region) munmap(pe->region, pe->cfg.tensor_region_bytes);
    if (pe->weight_fd >= 0) close(pe->weight_fd);
    free(pe->resident_bitmap);
    free(pe);
}

int paging_engine_ensure_resident(paging_engine_t *pe, uint64_t page_index, uint64_t now_ns) {
    if (page_index >= pe->n_pages) return -1;

    if (!bitmap_get(pe->resident_bitmap, page_index)) {
        uint8_t *page_ptr = (uint8_t *)pe->region + page_index * pe->cfg.page_size;
        if (pe->uffd >= 0) {
            /* Deliberately touch one byte: if this page is genuinely
             * missing, the kernel blocks this thread and delivers a fault
             * event to fault_thread_main(), which resolves it via
             * UFFDIO_COPY and unblocks us transparently. */
            volatile uint8_t touch = page_ptr[0];
            (void)touch;
        } else {
            uint8_t *tmp = malloc(pe->cfg.page_size);
            resolve_page_content(pe, page_index, tmp);
            memcpy(page_ptr, tmp, pe->cfg.page_size);
            free(tmp);
        }
        bitmap_set(pe->resident_bitmap, page_index);
    }

    uint64_t page_id = make_page_id(pe->cfg.node_id, page_index);
    int64_t slot = shm_pagetable_put(pe->table, page_id, /*tier=*/1 /* WIRE_TIER_LOCAL_DRAM */, now_ns);
    if (slot < 0) return -1; /* shared table/pool full -- caller should evict and retry */

    void *dst = dram_pool_slot(pe->pool, slot);
    if (dst) memcpy(dst, (uint8_t *)pe->region + page_index * pe->cfg.page_size, pe->cfg.page_size);
    return 0;
}

size_t paging_engine_evict_lru(paging_engine_t *pe, size_t count) {
    uint64_t *victims = malloc(count * sizeof(uint64_t));
    size_t n = shm_pagetable_find_lru(pe->table, count, victims);
    for (size_t i = 0; i < n; i++) {
        shm_pagetable_del(pe->table, victims[i]);
        uint64_t page_index = victims[i] & 0xFFFFFFFFu; /* see make_page_id() */
        if (page_index < pe->n_pages) {
            bitmap_clear(pe->resident_bitmap, page_index);
            /* MADV_DONTNEED frees the physical page and, for a uffd-
             * registered MISSING-mode region, makes the range absent
             * again -- the next touch re-faults instead of silently
             * reading stale content. In fallback mode it's just a memory
             * reclaim; ensure_resident's bitmap check is what makes the
             * next call re-read the page either way. */
            madvise((uint8_t *)pe->region + page_index * pe->cfg.page_size, pe->cfg.page_size, MADV_DONTNEED);
        }
    }
    free(victims);
    if (n > 0) clog_info("swapd: evicted %zu LRU page(s)", n);
    return n;
}

size_t paging_engine_resident_count(paging_engine_t *pe) { return shm_pagetable_count(pe->table); }
size_t paging_engine_capacity_pages(const paging_engine_t *pe) { return shm_pagetable_capacity(pe->table); }
int paging_engine_using_real_uffd(const paging_engine_t *pe) { return pe->uffd >= 0; }
