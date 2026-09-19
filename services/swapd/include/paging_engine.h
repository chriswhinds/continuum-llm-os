/* paging_engine.h -- swapd's core: a private anonymous mmap region for this
 * node's assigned tensor shard, demand-paged from the local NVMe-backed
 * weight file (ARCH-001 §05, Fig. 3's tiered resolution), with resolved
 * pages published into the shared dram_pool/shm_pagetable so node-agentd
 * (serving remote peers) and shard-execd (local zero-copy reads) can see
 * them without going through swapd again.
 *
 * Two operating modes, chosen automatically at startup:
 *
 *   - Real userfaultfd: the region is registered with the kernel's
 *     userfaultfd facility. A dedicated thread blocks reading fault
 *     events and resolves each one with UFFDIO_COPY. This is what
 *     ARCH-002 describes and is used whenever the process has permission
 *     to create a userfaultfd (root, or CAP_SYS_PTRACE, or
 *     vm.unprivileged_userfaultfd=1).
 *
 *   - Fallback simulation: on a dev machine without that permission
 *     (unprivileged userfaultfd is disabled by default on most modern
 *     distributions, including the one this reference build was
 *     developed on), userfaultfd(2) fails with EPERM. paging_engine logs
 *     this once, loudly, and switches to directly pread()ing a page's
 *     bytes into the region itself when first requested, tracked by its
 *     own per-page resident bitmap. The *observable* behavior at the
 *     paging_engine_ensure_resident() API is identical either way; only
 *     the mechanism -- a real kernel page fault vs. an explicit copy --
 *     differs. This keeps the reference build runnable out of the box
 *     while still shipping the real mechanism for the target hardware.
 */
#ifndef CONTINUUM_PAGING_ENGINE_H
#define CONTINUUM_PAGING_ENGINE_H

#include <stdint.h>

#include "dram_pool.h"
#include "shm_pagetable.h"
#include "swapd_config.h"

typedef struct paging_engine paging_engine_t;

paging_engine_t *paging_engine_create(const swapd_config_t *cfg, shm_pagetable_t *table, dram_pool_t *pool);
void paging_engine_destroy(paging_engine_t *pe);

/* Ensures the page at `page_index` (0-based within the tensor region) is
 * resident and published in the shared table/pool, refreshing its
 * last-access time either way. Returns 0 on success, -1 if page_index is
 * out of range or (in the remote-fallback path) unreachable everywhere. */
int paging_engine_ensure_resident(paging_engine_t *pe, uint64_t page_index, uint64_t now_ns);

/* Evicts the `count` least-recently-touched resident pages (ARCH-002 §05;
 * see the module comment in paging_engine.c for why this is plain LRU
 * rather than the doc's LFU+recency blend). Returns the number evicted. */
size_t paging_engine_evict_lru(paging_engine_t *pe, size_t count);

size_t paging_engine_resident_count(paging_engine_t *pe);
size_t paging_engine_capacity_pages(const paging_engine_t *pe);
int paging_engine_using_real_uffd(const paging_engine_t *pe);

#endif
