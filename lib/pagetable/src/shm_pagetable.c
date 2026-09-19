#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "shm_pagetable.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t lock;
    uint64_t capacity;
    uint64_t count;
} shm_header_t;

struct shm_pagetable {
    int fd;
    void *base;
    size_t size;
    shm_header_t *hdr;
    shm_page_entry_t *entries;
};

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static uint64_t hash_key(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return key;
}

shm_pagetable_t *shm_pagetable_open(const char *name, size_t capacity, int is_creator) {
    int flags = is_creator ? (O_CREAT | O_RDWR) : O_RDWR;
    int fd = shm_open(name, flags, 0600);
    if (fd < 0) return NULL;

    size_t size;
    if (is_creator) {
        capacity = next_pow2(capacity < 8 ? 8 : capacity);
        size = sizeof(shm_header_t) + capacity * sizeof(shm_page_entry_t);
        if (ftruncate(fd, (off_t)size) != 0) {
            close(fd);
            return NULL;
        }
    } else {
        struct stat st;
        if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(shm_header_t)) {
            close(fd);
            return NULL;
        }
        size = (size_t)st.st_size;
    }

    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    shm_pagetable_t *pt = calloc(1, sizeof(*pt));
    pt->fd = fd;
    pt->base = base;
    pt->size = size;
    pt->hdr = base;
    pt->entries = (shm_page_entry_t *)((uint8_t *)base + sizeof(shm_header_t));

    if (is_creator) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        /* Robust: if node-agentd crashes while holding the lock, swapd
         * doesn't wedge forever waiting on a dead owner's mutex. */
        pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        pthread_mutex_init(&pt->hdr->lock, &attr);
        pthread_mutexattr_destroy(&attr);

        pt->hdr->capacity = capacity;
        pt->hdr->count = 0;
        memset(pt->entries, 0, capacity * sizeof(shm_page_entry_t));
    }

    return pt;
}

void shm_pagetable_close(shm_pagetable_t *pt, const char *name, int unlink_name) {
    if (!pt) return;
    munmap(pt->base, pt->size);
    close(pt->fd);
    if (unlink_name && name) shm_unlink(name);
    free(pt);
}

static void lock_table(shm_pagetable_t *pt) {
    int rc = pthread_mutex_lock(&pt->hdr->lock);
    if (rc == EOWNERDEAD) {
        /* The previous holder (e.g. a crashed node-agentd) died mid-update.
         * The table's entries are still structurally valid (each write
         * below is a single struct assignment), so just mark the mutex
         * consistent and carry on rather than losing the whole table. */
        pthread_mutex_consistent(&pt->hdr->lock);
    }
}

int64_t shm_pagetable_put(shm_pagetable_t *pt, uint64_t page_id, uint8_t tier, uint64_t now_ns) {
    lock_table(pt);
    uint64_t cap = pt->hdr->capacity;
    uint64_t mask = cap - 1;
    uint64_t idx = hash_key(page_id) & mask;
    uint64_t first_tombstone = UINT64_MAX;

    for (uint64_t probes = 0; probes < cap; probes++) {
        shm_page_entry_t *e = &pt->entries[idx];
        if (e->state == PAGE_STATE_EMPTY) {
            uint64_t target = (first_tombstone != UINT64_MAX) ? first_tombstone : idx;
            shm_page_entry_t *slot = &pt->entries[target];
            slot->page_id = page_id;
            slot->last_access_ns = now_ns;
            slot->tier = tier;
            slot->state = PAGE_STATE_RESIDENT;
            pt->hdr->count++;
            pthread_mutex_unlock(&pt->hdr->lock);
            return (int64_t)target;
        }
        if (e->state == PAGE_STATE_TOMBSTONE) {
            if (first_tombstone == UINT64_MAX) first_tombstone = idx;
        } else if (e->page_id == page_id) {
            e->last_access_ns = now_ns;
            e->tier = tier;
            pthread_mutex_unlock(&pt->hdr->lock);
            return (int64_t)idx;
        }
        idx = (idx + 1) & mask;
    }
    pthread_mutex_unlock(&pt->hdr->lock);
    return -1; /* table full */
}

int shm_pagetable_get(shm_pagetable_t *pt, uint64_t page_id, shm_page_entry_t *out, int64_t *out_slot) {
    lock_table(pt);
    uint64_t cap = pt->hdr->capacity;
    uint64_t mask = cap - 1;
    uint64_t idx = hash_key(page_id) & mask;

    for (uint64_t probes = 0; probes < cap; probes++) {
        shm_page_entry_t *e = &pt->entries[idx];
        if (e->state == PAGE_STATE_EMPTY) break;
        if (e->state == PAGE_STATE_RESIDENT && e->page_id == page_id) {
            if (out) *out = *e;
            if (out_slot) *out_slot = (int64_t)idx;
            pthread_mutex_unlock(&pt->hdr->lock);
            return 1;
        }
        idx = (idx + 1) & mask;
    }
    pthread_mutex_unlock(&pt->hdr->lock);
    return 0;
}

int shm_pagetable_del(shm_pagetable_t *pt, uint64_t page_id) {
    lock_table(pt);
    uint64_t cap = pt->hdr->capacity;
    uint64_t mask = cap - 1;
    uint64_t idx = hash_key(page_id) & mask;

    for (uint64_t probes = 0; probes < cap; probes++) {
        shm_page_entry_t *e = &pt->entries[idx];
        if (e->state == PAGE_STATE_EMPTY) break;
        if (e->state == PAGE_STATE_RESIDENT && e->page_id == page_id) {
            e->state = PAGE_STATE_TOMBSTONE;
            pt->hdr->count--;
            pthread_mutex_unlock(&pt->hdr->lock);
            return 1;
        }
        idx = (idx + 1) & mask;
    }
    pthread_mutex_unlock(&pt->hdr->lock);
    return 0;
}

size_t shm_pagetable_capacity(const shm_pagetable_t *pt) { return pt->hdr->capacity; }

size_t shm_pagetable_count(shm_pagetable_t *pt) {
    lock_table(pt);
    size_t c = pt->hdr->count;
    pthread_mutex_unlock(&pt->hdr->lock);
    return c;
}

typedef struct {
    uint64_t page_id;
    uint64_t last_access_ns;
} lru_candidate_t;

static int cmp_candidate(const void *a, const void *b) {
    const lru_candidate_t *ca = a, *cb = b;
    if (ca->last_access_ns < cb->last_access_ns) return -1;
    if (ca->last_access_ns > cb->last_access_ns) return 1;
    return 0;
}

size_t shm_pagetable_find_lru(shm_pagetable_t *pt, size_t k, uint64_t *out_page_ids) {
    lock_table(pt);
    uint64_t cap = pt->hdr->capacity;
    lru_candidate_t *all = malloc(cap * sizeof(lru_candidate_t));
    size_t n = 0;
    for (uint64_t i = 0; i < cap; i++) {
        if (pt->entries[i].state == PAGE_STATE_RESIDENT) {
            all[n].page_id = pt->entries[i].page_id;
            all[n].last_access_ns = pt->entries[i].last_access_ns;
            n++;
        }
    }
    pthread_mutex_unlock(&pt->hdr->lock);

    qsort(all, n, sizeof(lru_candidate_t), cmp_candidate);
    size_t out_n = (k < n) ? k : n;
    for (size_t i = 0; i < out_n; i++) out_page_ids[i] = all[i].page_id;
    free(all);
    return out_n;
}
