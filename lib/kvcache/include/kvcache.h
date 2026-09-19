/* kvcache.h -- token-prefix KV cache, linked directly into shard-execd
 * (ARCH-002 §02, §05) rather than run as its own daemon, to keep the
 * hottest path in the decode loop free of an extra IPC hop.
 *
 * Implementation note / scope: this is a token-level trie (one edge per
 * token id, first-child/next-sibling representation) rather than a
 * byte-compressed PATRICIA/radix tree. It gives the same prefix-sharing
 * behavior ARCH-001 §09 describes -- two sequences with a shared system
 * prompt share the same cached path -- at the cost of one node per token
 * instead of one node per diverging run of tokens. Edge compression is a
 * real future optimization, left out here to keep the reference
 * implementation's core logic easy to verify.
 */
#ifndef CONTINUUM_KVCACHE_H
#define CONTINUUM_KVCACHE_H

#include <stddef.h>
#include <stdint.h>

typedef struct kvcache kvcache_t;

/* capacity is the maximum number of trie nodes (~= cached tokens across all
 * resident sequences) held at once -- a fixed arena, no growth. */
kvcache_t *kvcache_create(size_t capacity);
void kvcache_destroy(kvcache_t *kc);

/* Extends the cache with a token sequence and its per-token KV page ids
 * (page_ids[i] is where shard-execd should find/put the KV state produced
 * after consuming tokens[i], given tokens[0..i] as context). Existing
 * shared prefix nodes are reused, not duplicated. now_ns timestamps every
 * node touched (new or reused) for the idle-eviction pass below. Returns 0
 * on success, -1 if the arena is full (caller should evict first). */
int kvcache_insert(kvcache_t *kc, const uint32_t *tokens, const uint64_t *page_ids, size_t n, uint64_t now_ns);

/* Finds the longest prefix of tokens[0..n) already present, writing the
 * matched page ids into out_page_ids (caller-allocated, >= n entries) and
 * stamping now_ns as the last-access time of every node on the matched
 * path. Returns the number of tokens matched (0 if none, e.g. an empty
 * cache). */
size_t kvcache_lookup_prefix(kvcache_t *kc, const uint32_t *tokens, size_t n, uint64_t *out_page_ids, uint64_t now_ns);

typedef void (*kvcache_evict_cb)(uint64_t page_id, void *user);

/* Evicts every subtree whose most-recent access (including all of its
 * descendants) is older than now_ns - max_idle_ns. cb is invoked once per
 * evicted node with the page id that node was holding, so the caller (the
 * swap manager) can release the backing tier-3/tier-4 storage. Returns the
 * number of nodes evicted. */
size_t kvcache_evict_idle(kvcache_t *kc, uint64_t now_ns, uint64_t max_idle_ns,
                           kvcache_evict_cb cb, void *user);

size_t kvcache_node_count(const kvcache_t *kc);
size_t kvcache_capacity(const kvcache_t *kc);

#endif
