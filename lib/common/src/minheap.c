#include "minheap.h"

#include <stdlib.h>

struct minheap {
    minheap_item_t *items;
    size_t size;
    size_t cap;
};

minheap_t *minheap_create(size_t initial_capacity) {
    if (initial_capacity < 16) initial_capacity = 16;
    minheap_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->items = calloc(initial_capacity, sizeof(minheap_item_t));
    h->cap = initial_capacity;
    h->size = 0;
    return h;
}

void minheap_destroy(minheap_t *h) {
    if (!h) return;
    free(h->items);
    free(h);
}

static void swap(minheap_item_t *a, minheap_item_t *b) {
    minheap_item_t t = *a;
    *a = *b;
    *b = t;
}

static void sift_up(minheap_t *h, size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (h->items[parent].priority <= h->items[i].priority) break;
        swap(&h->items[parent], &h->items[i]);
        i = parent;
    }
}

static void sift_down(minheap_t *h, size_t i) {
    for (;;) {
        size_t left = 2 * i + 1, right = 2 * i + 2, smallest = i;
        if (left < h->size && h->items[left].priority < h->items[smallest].priority) smallest = left;
        if (right < h->size && h->items[right].priority < h->items[smallest].priority) smallest = right;
        if (smallest == i) break;
        swap(&h->items[i], &h->items[smallest]);
        i = smallest;
    }
}

int minheap_push(minheap_t *h, int64_t priority, uint64_t id, void *data) {
    if (h->size == h->cap) {
        size_t new_cap = h->cap * 2;
        minheap_item_t *n = realloc(h->items, new_cap * sizeof(minheap_item_t));
        if (!n) return -1;
        h->items = n;
        h->cap = new_cap;
    }
    h->items[h->size] = (minheap_item_t){.priority = priority, .id = id, .data = data};
    sift_up(h, h->size);
    h->size++;
    return 0;
}

int minheap_pop(minheap_t *h, minheap_item_t *out) {
    if (h->size == 0) return 0;
    if (out) *out = h->items[0];
    h->size--;
    h->items[0] = h->items[h->size];
    sift_down(h, 0);
    return 1;
}

int minheap_peek(const minheap_t *h, minheap_item_t *out) {
    if (h->size == 0) return 0;
    if (out) *out = h->items[0];
    return 1;
}

size_t minheap_size(const minheap_t *h) { return h->size; }
