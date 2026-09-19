#include "kvcache.h"

#include <stdlib.h>
#include <string.h>

#define KV_NIL (-1)

typedef struct {
    uint32_t token;
    uint64_t page_id;
    uint64_t last_access_ns;
    int32_t parent;
    int32_t first_child;
    int32_t next_sibling;
    int used;
} trie_node_t;

struct kvcache {
    trie_node_t *nodes;
    size_t cap;
    size_t count;      /* used nodes, including root */
    int32_t free_head;  /* singly-linked free list through next_sibling */
};

static int32_t alloc_node(kvcache_t *kc) {
    if (kc->free_head == KV_NIL) return KV_NIL;
    int32_t idx = kc->free_head;
    kc->free_head = kc->nodes[idx].next_sibling;
    trie_node_t *n = &kc->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->parent = KV_NIL;
    n->first_child = KV_NIL;
    n->next_sibling = KV_NIL;
    n->used = 1;
    kc->count++;
    return idx;
}

static void free_node(kvcache_t *kc, int32_t idx) {
    kc->nodes[idx].used = 0;
    kc->nodes[idx].next_sibling = kc->free_head;
    kc->free_head = idx;
    kc->count--;
}

kvcache_t *kvcache_create(size_t capacity) {
    if (capacity < 2) capacity = 2;
    kvcache_t *kc = calloc(1, sizeof(*kc));
    if (!kc) return NULL;
    kc->nodes = calloc(capacity, sizeof(trie_node_t));
    if (!kc->nodes) {
        free(kc);
        return NULL;
    }
    kc->cap = capacity;

    /* Build the free list over every slot, then carve out node 0 as root. */
    for (size_t i = 0; i < capacity; i++) {
        kc->nodes[i].next_sibling = (int32_t)((i + 1 < capacity) ? (int32_t)(i + 1) : KV_NIL);
        kc->nodes[i].used = 0;
    }
    kc->free_head = 0;
    kc->count = 0;

    int32_t root = alloc_node(kc);
    kc->nodes[root].parent = KV_NIL;
    kc->nodes[root].first_child = KV_NIL;
    /* root is always node 0 by construction */
    return kc;
}

void kvcache_destroy(kvcache_t *kc) {
    if (!kc) return;
    free(kc->nodes);
    free(kc);
}

static int32_t find_child(kvcache_t *kc, int32_t parent, uint32_t token) {
    for (int32_t c = kc->nodes[parent].first_child; c != KV_NIL; c = kc->nodes[c].next_sibling) {
        if (kc->nodes[c].token == token) return c;
    }
    return KV_NIL;
}

int kvcache_insert(kvcache_t *kc, const uint32_t *tokens, const uint64_t *page_ids, size_t n, uint64_t now_ns) {
    int32_t cur = 0; /* root */
    for (size_t i = 0; i < n; i++) {
        int32_t child = find_child(kc, cur, tokens[i]);
        if (child == KV_NIL) {
            child = alloc_node(kc);
            if (child == KV_NIL) return -1; /* arena full */
            trie_node_t *cn = &kc->nodes[child];
            cn->token = tokens[i];
            cn->page_id = page_ids[i];
            cn->parent = cur;
            cn->next_sibling = kc->nodes[cur].first_child;
            kc->nodes[cur].first_child = child;
        }
        /* Path already cached (shared prefix) or freshly created either
         * way: this position was just used, so stamp it. */
        kc->nodes[child].last_access_ns = now_ns;
        cur = child;
    }
    return 0;
}

size_t kvcache_lookup_prefix(kvcache_t *kc, const uint32_t *tokens, size_t n, uint64_t *out_page_ids, uint64_t now_ns) {
    int32_t cur = 0;
    size_t matched = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t child = find_child(kc, cur, tokens[i]);
        if (child == KV_NIL) break;
        out_page_ids[i] = kc->nodes[child].page_id;
        kc->nodes[child].last_access_ns = now_ns;
        cur = child;
        matched++;
    }
    return matched;
}

/* ---- idle-subtree eviction ------------------------------------------------
 * Two iterative passes over an explicit stack (no real recursion, so a very
 * long conversation can't blow the C stack):
 *   1. Bottom-up: compute effective[node] = max(own last_access, every
 *      descendant's last_access) via iterative post-order.
 *   2. Top-down from the root: if a node's effective time is older than the
 *      cutoff, evict its entire subtree in one iterative traversal and do
 *      not descend into it further; otherwise keep walking its children.
 */

typedef struct {
    int32_t node;
    int visited;
} stack_frame_t;

static void compute_effective(kvcache_t *kc, uint64_t *effective) {
    stack_frame_t *stack = malloc(kc->cap * sizeof(stack_frame_t));
    size_t sp = 0;
    stack[sp++] = (stack_frame_t){.node = 0, .visited = 0};

    while (sp > 0) {
        stack_frame_t *top = &stack[sp - 1];
        if (!top->visited) {
            top->visited = 1;
            for (int32_t c = kc->nodes[top->node].first_child; c != KV_NIL; c = kc->nodes[c].next_sibling) {
                stack[sp++] = (stack_frame_t){.node = c, .visited = 0};
            }
        } else {
            int32_t node = top->node;
            uint64_t eff = kc->nodes[node].last_access_ns;
            for (int32_t c = kc->nodes[node].first_child; c != KV_NIL; c = kc->nodes[c].next_sibling) {
                if (effective[c] > eff) eff = effective[c];
            }
            effective[node] = eff;
            sp--;
        }
    }
    free(stack);
}

static void unlink_child(kvcache_t *kc, int32_t parent, int32_t child) {
    int32_t *slot = &kc->nodes[parent].first_child;
    while (*slot != KV_NIL && *slot != child) slot = &kc->nodes[*slot].next_sibling;
    if (*slot == child) *slot = kc->nodes[child].next_sibling;
}

static size_t evict_subtree(kvcache_t *kc, int32_t root, kvcache_evict_cb cb, void *user) {
    int32_t *stack = malloc(kc->cap * sizeof(int32_t));
    size_t sp = 0;
    stack[sp++] = root;
    size_t evicted = 0;

    while (sp > 0) {
        int32_t node = stack[--sp];
        for (int32_t c = kc->nodes[node].first_child; c != KV_NIL; c = kc->nodes[c].next_sibling) {
            stack[sp++] = c;
        }
        if (cb) cb(kc->nodes[node].page_id, user);
        free_node(kc, node);
        evicted++;
    }
    free(stack);
    return evicted;
}

size_t kvcache_evict_idle(kvcache_t *kc, uint64_t now_ns, uint64_t max_idle_ns,
                           kvcache_evict_cb cb, void *user) {
    if (max_idle_ns >= now_ns) return 0; /* nothing can be older than "the beginning of time" */
    uint64_t cutoff = now_ns - max_idle_ns;

    uint64_t *effective = calloc(kc->cap, sizeof(uint64_t));
    compute_effective(kc, effective);

    int32_t *stack = malloc(kc->cap * sizeof(int32_t));
    size_t sp = 0;
    /* Seed with root's children -- the root itself is structural and never evicted. */
    for (int32_t c = kc->nodes[0].first_child; c != KV_NIL; c = kc->nodes[c].next_sibling) {
        stack[sp++] = c;
    }

    size_t total_evicted = 0;
    while (sp > 0) {
        int32_t node = stack[--sp];
        if (effective[node] < cutoff) {
            unlink_child(kc, kc->nodes[node].parent, node);
            total_evicted += evict_subtree(kc, node, cb, user);
        } else {
            for (int32_t c = kc->nodes[node].first_child; c != KV_NIL; c = kc->nodes[c].next_sibling) {
                stack[sp++] = c;
            }
        }
    }

    free(stack);
    free(effective);
    return total_evicted;
}

size_t kvcache_node_count(const kvcache_t *kc) { return kc->count; }
size_t kvcache_capacity(const kvcache_t *kc) { return kc->cap; }
