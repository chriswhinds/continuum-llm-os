#include "kvcache.h"

#include <assert.h>
#include <stdio.h>

static int evicted_count;
static void on_evict(uint64_t page_id, void *user) {
    (void)page_id;
    (void)user;
    evicted_count++;
}

static void test_shared_prefix(void) {
    kvcache_t *kc = kvcache_create(64);

    uint32_t seq_a[] = {10, 20, 30, 40};
    uint64_t pages_a[] = {1001, 1002, 1003, 1004};
    assert(kvcache_insert(kc, seq_a, pages_a, 4, 100) == 0);

    /* seq_b shares the first three tokens with seq_a, then diverges. */
    uint32_t seq_b[] = {10, 20, 30, 99};
    uint64_t pages_b[] = {1001, 1002, 1003, 2001};
    assert(kvcache_insert(kc, seq_b, pages_b, 4, 101) == 0);

    /* The shared prefix must be ONE path in the trie, not two: inserting
     * both sequences should cost only 5 nodes (root + 10,20,30 shared +
     * 40 + 99), never 9 (root + two full paths of 4). */
    assert(kvcache_node_count(kc) == 6); /* root + {10,20,30} + {40} + {99} */

    uint64_t out[8];
    size_t matched = kvcache_lookup_prefix(kc, seq_b, 4, out, 102);
    assert(matched == 4);
    assert(out[0] == 1001 && out[1] == 1002 && out[2] == 1003 && out[3] == 2001);

    uint32_t seq_c[] = {10, 20, 30};
    size_t matched_c = kvcache_lookup_prefix(kc, seq_c, 3, out, 103);
    assert(matched_c == 3);
    assert(out[2] == 1003);

    kvcache_destroy(kc);
    printf("ok: shared prefix dedup + lookup\n");
}

static void test_idle_eviction(void) {
    kvcache_t *kc = kvcache_create(64);

    uint32_t seq_old[] = {1, 2, 3};
    uint64_t pages_old[] = {1, 2, 3};
    kvcache_insert(kc, seq_old, pages_old, 3, 1000); /* last touched at t=1000 */

    uint32_t seq_new[] = {1, 2, 9};
    uint64_t pages_new[] = {1, 2, 9};
    kvcache_insert(kc, seq_new, pages_new, 3, 5000); /* branch touched at t=5000 */

    /* {1,2} is shared: seq_new's insert re-stamps it to 5000, so only the
     * {3} branch (still at 1000) should be evictable at now=6000 with a
     * 2000ns idle window (cutoff=4000). */
    evicted_count = 0;
    size_t evicted = kvcache_evict_idle(kc, 6000, 2000, on_evict, NULL);
    assert(evicted == 1);
    assert(evicted_count == 1);

    uint64_t out[8];
    /* {1,2,3} should now miss at the third token (evicted), {1,2,9} still hits fully. */
    size_t m1 = kvcache_lookup_prefix(kc, seq_old, 3, out, 6001);
    assert(m1 == 2);
    size_t m2 = kvcache_lookup_prefix(kc, seq_new, 3, out, 6002);
    assert(m2 == 3);

    kvcache_destroy(kc);
    printf("ok: idle subtree eviction preserves fresher siblings\n");
}

static void test_capacity_exhaustion(void) {
    kvcache_t *kc = kvcache_create(4); /* room for root + 3 nodes only */
    uint32_t seq[] = {1, 2, 3, 4};
    uint64_t pages[] = {1, 2, 3, 4};
    int rc = kvcache_insert(kc, seq, pages, 4, 1);
    assert(rc == -1); /* the 4th token node can't fit */
    kvcache_destroy(kc);
    printf("ok: capacity exhaustion reported, no crash/corruption\n");
}

int main(void) {
    test_shared_prefix();
    test_idle_eviction();
    test_capacity_exhaustion();
    printf("all kvcache tests passed\n");
    return 0;
}
