/* dram_pool.h -- the node's actual tensor bytes: a POSIX shared-memory
 * segment of `capacity * page_size` bytes, where `capacity` matches the
 * companion shm_pagetable's capacity (see shm_pagetable.h's header note --
 * a page's table slot index is also its offset here, divided by
 * page_size). node-agentd creates it; swapd writes into it on page-in;
 * shard-execd reads tensor bytes out of it directly, zero-copy. */
#ifndef CONTINUUM_DRAM_POOL_H
#define CONTINUUM_DRAM_POOL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    void *base;
    size_t size;
    size_t page_size;
} dram_pool_t;

/* is_creator=1 (node-agentd) actually sizes and zero-fills the segment;
 * others (swapd, shard-execd) attach to whatever size it already is. */
int dram_pool_open(dram_pool_t *pool, const char *name, size_t capacity_slots, size_t page_size, int is_creator);
void dram_pool_close(dram_pool_t *pool, const char *name, int unlink_name);

/* Returns a pointer to slot `slot_index`'s page_size-byte region within the
 * mapping, or NULL if out of range. */
void *dram_pool_slot(dram_pool_t *pool, int64_t slot_index);

#endif
