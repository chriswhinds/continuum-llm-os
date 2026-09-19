/* Exercises paging_engine against a real temp file standing in for a
 * node's NVMe-backed weight shard: ensure_resident() must fetch the right
 * bytes, publish them to the shared table/pool so a second reader can see
 * them without re-resolving, and eviction must make a page's slot
 * reusable and force a fresh resolve on the next touch.
 *
 * This runs whichever paging mode the host machine permits (see
 * paging_engine.h) -- on the machine this was developed on, unprivileged
 * userfaultfd is disabled, so this test exercises the fallback path. Both
 * paths share the exact same ensure_resident() logic after the initial
 * "get correct bytes into the region" step, so this is still a real test
 * of the shared-publish and eviction behavior either way.
 */
#include "paging_engine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SHM_PT "/continuum-pt-test-swapd"
#define SHM_POOL "/continuum-pool-test-swapd"

int main(void) {
    const uint64_t page_size = 4096;
    const uint64_t n_pages = 8;
    const char *weight_path = "/tmp/continuum_paging_engine_test.weights";

    /* Build a weight file where page i is filled with byte value i, so a
     * fetched page's content trivially proves which page it is. */
    FILE *f = fopen(weight_path, "wb");
    uint8_t *buf = malloc(page_size);
    for (uint64_t i = 0; i < n_pages; i++) {
        memset(buf, (int)i, page_size);
        fwrite(buf, 1, page_size, f);
    }
    fclose(f);

    swapd_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    strncpy(cfg.weight_file, weight_path, sizeof(cfg.weight_file) - 1);
    cfg.tensor_region_bytes = n_pages * page_size;
    cfg.page_size = page_size;
    cfg.dram_capacity_pages = 4; /* smaller than n_pages -- forces real eviction pressure */
    cfg.high_watermark_pct = 85;

    shm_pagetable_t *table = shm_pagetable_open(SHM_PT, cfg.dram_capacity_pages, 1);
    assert(table);
    dram_pool_t pool;
    assert(dram_pool_open(&pool, SHM_POOL, shm_pagetable_capacity(table), page_size, 1) == 0);

    paging_engine_t *pe = paging_engine_create(&cfg, table, &pool);
    assert(pe);
    printf("info: running with %s userfaultfd\n", paging_engine_using_real_uffd(pe) ? "real" : "simulated");

    /* Fetch page 2 and check its bytes. */
    assert(paging_engine_ensure_resident(pe, 2, 1000) == 0);
    shm_page_entry_t e;
    int64_t slot = -1;
    uint64_t page_id_2 = ((uint64_t)cfg.node_id << 32) | 2;
    assert(shm_pagetable_get(table, page_id_2, &e, &slot) == 1);
    uint8_t *bytes = dram_pool_slot(&pool, slot);
    for (uint64_t i = 0; i < page_size; i++) assert(bytes[i] == 2);
    printf("ok: fetched page content matches expected byte pattern\n");

    /* Page 2 is already resident from the fetch above, at ts=1000 -- the
     * oldest so far. Round out the DRAM budget (4 pages) with three more,
     * each newer, so page 2 is now unambiguously the LRU victim. */
    assert(paging_engine_ensure_resident(pe, 0, 1001) == 0);
    assert(paging_engine_ensure_resident(pe, 1, 1002) == 0);
    assert(paging_engine_ensure_resident(pe, 3, 1003) == 0);
    assert(shm_pagetable_count(table) == 4);
    printf("ok: DRAM budget filled to capacity with distinct pages\n");

    /* Evict the single oldest (page 2, last_access=1000) and confirm it's
     * gone from the shared table but everything newer survives. */
    size_t evicted = paging_engine_evict_lru(pe, 1);
    assert(evicted == 1);
    assert(shm_pagetable_get(table, page_id_2, &e, NULL) == 0);
    uint64_t page_id_0 = ((uint64_t)cfg.node_id << 32) | 0;
    assert(shm_pagetable_get(table, page_id_0, &e, NULL) == 1);
    printf("ok: LRU eviction removed the oldest page and kept the rest\n");

    /* Touching the evicted page again must succeed and re-publish it with
     * the correct content, proving eviction didn't corrupt anything. */
    assert(paging_engine_ensure_resident(pe, 2, 2000) == 0);
    int64_t slot2b = -1;
    assert(shm_pagetable_get(table, page_id_2, &e, &slot2b) == 1);
    uint8_t *bytes2b = dram_pool_slot(&pool, slot2b);
    for (uint64_t i = 0; i < page_size; i++) assert(bytes2b[i] == 2);
    printf("ok: re-fetch after eviction restores correct content\n");

    paging_engine_destroy(pe);
    dram_pool_close(&pool, SHM_POOL, 1);
    shm_pagetable_close(table, SHM_PT, 1);
    unlink(weight_path);

    printf("all paging_engine tests passed\n");
    return 0;
}
