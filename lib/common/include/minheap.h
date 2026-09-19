/* minheap.h -- binary min-heap keyed by a signed priority (lower = served
 * first). Used by schedulerd for request placement scoring (ARCH-002 §05). */
#ifndef CONTINUUM_MINHEAP_H
#define CONTINUUM_MINHEAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int64_t priority;
    uint64_t id;
    void *data;
} minheap_item_t;

typedef struct minheap minheap_t;

minheap_t *minheap_create(size_t initial_capacity);
void minheap_destroy(minheap_t *h);

int minheap_push(minheap_t *h, int64_t priority, uint64_t id, void *data);
/* Returns 1 and fills *out on success, 0 if the heap is empty. */
int minheap_pop(minheap_t *h, minheap_item_t *out);
int minheap_peek(const minheap_t *h, minheap_item_t *out);
size_t minheap_size(const minheap_t *h);

#endif
