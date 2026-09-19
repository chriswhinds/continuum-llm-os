/* hashtable.h -- fixed-capacity open-addressing hash table, u64 key -> fixed-size value.
 *
 * Every slot is pre-allocated at ht_create() time (one arena, no malloc churn
 * on the hot path) -- the design pattern ARCH-002 calls for in node-agentd's
 * page table and page-directoryd's directory state. Capacity does not grow;
 * ht_put() returns -1 if the table is full (load factor keeps this rare in
 * practice -- size the table for ~2x the expected entry count).
 */
#ifndef CONTINUUM_HASHTABLE_H
#define CONTINUUM_HASHTABLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct ht ht_t;

/* capacity is rounded up to the next power of two. value_size is the size in
 * bytes of the fixed value type stored per key (memcpy'd in and out). */
ht_t *ht_create(size_t capacity, size_t value_size);
void ht_destroy(ht_t *ht);

/* Inserts or overwrites. Returns 0 on success, -1 if the table is full. */
int ht_put(ht_t *ht, uint64_t key, const void *value);

/* Returns a pointer to the stored value (valid until the next ht_put/ht_del
 * that touches this slot), or NULL if not present. */
void *ht_get(ht_t *ht, uint64_t key);

/* Returns 1 if removed, 0 if the key wasn't present. */
int ht_del(ht_t *ht, uint64_t key);

size_t ht_count(const ht_t *ht);
size_t ht_capacity(const ht_t *ht);

typedef void (*ht_iter_fn)(uint64_t key, void *value, void *user);
void ht_foreach(ht_t *ht, ht_iter_fn fn, void *user);

#endif
