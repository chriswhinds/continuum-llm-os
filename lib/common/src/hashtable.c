#include "hashtable.h"

#include <stdlib.h>
#include <string.h>

typedef enum { SLOT_EMPTY = 0, SLOT_USED = 1, SLOT_TOMBSTONE = 2 } slot_state_t;

struct ht {
    size_t cap;        /* power of two */
    size_t mask;
    size_t value_size;
    size_t count;
    uint8_t *states;   /* cap entries of slot_state_t */
    uint64_t *keys;    /* cap entries */
    uint8_t *values;   /* cap * value_size bytes */
};

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* Fibonacci-ish hash of a u64 key -- fast, good-enough avalanche for a page/
 * node id keyspace that is not attacker-controlled. */
static uint64_t hash_key(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return key;
}

ht_t *ht_create(size_t capacity, size_t value_size) {
    if (capacity < 8) capacity = 8;
    size_t cap = next_pow2(capacity);

    ht_t *ht = calloc(1, sizeof(*ht));
    if (!ht) return NULL;
    ht->cap = cap;
    ht->mask = cap - 1;
    ht->value_size = value_size;
    ht->count = 0;
    ht->states = calloc(cap, sizeof(uint8_t));
    ht->keys = calloc(cap, sizeof(uint64_t));
    ht->values = calloc(cap, value_size);
    if (!ht->states || !ht->keys || !ht->values) {
        ht_destroy(ht);
        return NULL;
    }
    return ht;
}

void ht_destroy(ht_t *ht) {
    if (!ht) return;
    free(ht->states);
    free(ht->keys);
    free(ht->values);
    free(ht);
}

static size_t find_slot(ht_t *ht, uint64_t key, int *found) {
    size_t idx = hash_key(key) & ht->mask;
    size_t first_tombstone = (size_t)-1;
    for (size_t probes = 0; probes < ht->cap; probes++) {
        uint8_t st = ht->states[idx];
        if (st == SLOT_EMPTY) {
            *found = 0;
            return (first_tombstone != (size_t)-1) ? first_tombstone : idx;
        }
        if (st == SLOT_TOMBSTONE) {
            if (first_tombstone == (size_t)-1) first_tombstone = idx;
        } else if (ht->keys[idx] == key) {
            *found = 1;
            return idx;
        }
        idx = (idx + 1) & ht->mask;
    }
    *found = 0;
    return (size_t)-1; /* table full */
}

int ht_put(ht_t *ht, uint64_t key, const void *value) {
    int found = 0;
    size_t idx = find_slot(ht, key, &found);
    if (idx == (size_t)-1) return -1;
    if (!found) {
        ht->states[idx] = SLOT_USED;
        ht->keys[idx] = key;
        ht->count++;
    }
    memcpy(ht->values + idx * ht->value_size, value, ht->value_size);
    return 0;
}

void *ht_get(ht_t *ht, uint64_t key) {
    int found = 0;
    size_t idx = find_slot(ht, key, &found);
    if (!found || idx == (size_t)-1) return NULL;
    return ht->values + idx * ht->value_size;
}

int ht_del(ht_t *ht, uint64_t key) {
    int found = 0;
    size_t idx = find_slot(ht, key, &found);
    if (!found) return 0;
    ht->states[idx] = SLOT_TOMBSTONE;
    ht->count--;
    return 1;
}

size_t ht_count(const ht_t *ht) { return ht->count; }
size_t ht_capacity(const ht_t *ht) { return ht->cap; }

void ht_foreach(ht_t *ht, ht_iter_fn fn, void *user) {
    for (size_t i = 0; i < ht->cap; i++) {
        if (ht->states[i] == SLOT_USED) {
            fn(ht->keys[i], ht->values + i * ht->value_size, user);
        }
    }
}
