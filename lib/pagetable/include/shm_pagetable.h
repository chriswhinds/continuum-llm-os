/* shm_pagetable.h -- the local DRAM pool's page table as a POSIX shared-
 * memory segment, mapped by node-agentd (owner/writer of record), swapd
 * (the userfaultfd handler resolving a fault to a tier and an offset
 * without an IPC round trip on the hot path), and shard-execd (reading
 * resident tensor bytes directly).
 *
 * This is the "shared RAM" half of ARCH-001 §03 realized concretely at
 * node scope: one open-addressing table of fixed-size entries, living in a
 * single mmap() every process attaches to, guarded by a PTHREAD_PROCESS_
 * SHARED mutex. No pointers are ever stored in an entry -- only integers
 * and offsets -- so the same bytes are valid at whatever virtual address
 * each process happens to map the segment at.
 *
 * Design choice: the table doubles as the DRAM pool's slot allocator. A
 * page's table slot index *is* its storage slot -- shm_pagetable_put()
 * returns the index it landed on (after open-addressing probing), and the
 * caller stores/reads that page's tensor bytes at
 * `slot_index * page_size` in a separate, equally-sized shared DRAM pool
 * segment (see node-agentd's dram_pool.h). This avoids running a second,
 * independent shared-memory allocator alongside the lookup table -- one
 * structure does both jobs, and "insert into the table" and "reserve a
 * slot" are the same atomic operation instead of two that could race.
 */
#ifndef CONTINUUM_SHM_PAGETABLE_H
#define CONTINUUM_SHM_PAGETABLE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PAGE_STATE_EMPTY = 0,
    PAGE_STATE_RESIDENT = 1,
    PAGE_STATE_TOMBSTONE = 2,
} page_state_t;

#pragma pack(push, 1)
typedef struct {
    uint64_t page_id;
    uint64_t last_access_ns;
    uint8_t  tier;           /* wire_tier_t from wire.h, duplicated here to avoid a lib/wire dependency */
    uint8_t  state;          /* page_state_t */
    uint16_t reserved;
} shm_page_entry_t;
#pragma pack(pop)

typedef struct shm_pagetable shm_pagetable_t;

/* Opens (creating if `is_creator`) a shared page table under POSIX shm name
 * `name` (must start with '/', e.g. "/continuum-pt-node07"). `capacity` is
 * rounded up to the next power of two and is fixed for the table's
 * lifetime -- it is also the DRAM pool's slot count, so the pool segment
 * the caller creates alongside this must be `capacity * page_size` bytes.
 * Exactly one process (node-agentd) should pass is_creator=1; every other
 * attacher (swapd, shard-execd) passes 0 and must be started after the
 * creator has called this at least once. Returns NULL on failure. */
shm_pagetable_t *shm_pagetable_open(const char *name, size_t capacity, int is_creator);

/* unlink_name: pass 1 from the owning process on final shutdown to remove
 * the /dev/shm entry; other attachers should pass 0. */
void shm_pagetable_close(shm_pagetable_t *pt, const char *name, int unlink_name);

/* Inserts (or refreshes, if already present) page_id and returns the slot
 * index it occupies in the table -- also its offset in the companion DRAM
 * pool segment, as `slot_index * page_size`. Returns -1 if the table is
 * full. Note: for an existing key this returns its ALREADY-assigned slot
 * (the tensor bytes there are left untouched by this call), not a new one
 * -- callers must check whether the return is a fresh slot (write the
 * bytes) or an existing one (bytes are presumably already correct) via
 * shm_pagetable_get() first if that distinction matters. */
int64_t shm_pagetable_put(shm_pagetable_t *pt, uint64_t page_id, uint8_t tier, uint64_t now_ns);
/* Returns 1 and fills *out plus *out_slot if found, 0 otherwise. out_slot may be NULL. */
int shm_pagetable_get(shm_pagetable_t *pt, uint64_t page_id, shm_page_entry_t *out, int64_t *out_slot);
int shm_pagetable_del(shm_pagetable_t *pt, uint64_t page_id);

size_t shm_pagetable_capacity(const shm_pagetable_t *pt);
size_t shm_pagetable_count(shm_pagetable_t *pt);

/* Fills out_page_ids with up to `k` of the least-recently-touched resident
 * page ids (by last_access_ns, ascending), for LRU eviction. Returns the
 * number actually written (may be less than k if fewer pages are
 * resident). O(capacity log capacity) -- a full scan-and-sort, which is
 * fine at this table's reference scale (thousands of entries) but not
 * meant to be called on a hot path. */
size_t shm_pagetable_find_lru(shm_pagetable_t *pt, size_t k, uint64_t *out_page_ids);

#endif
