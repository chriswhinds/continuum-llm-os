#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "dram_pool.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int dram_pool_open(dram_pool_t *pool, const char *name, size_t capacity_slots, size_t page_size, int is_creator) {
    memset(pool, 0, sizeof(*pool));
    int flags = is_creator ? (O_CREAT | O_RDWR) : O_RDWR;
    int fd = shm_open(name, flags, 0600);
    if (fd < 0) return -1;

    size_t size;
    if (is_creator) {
        size = capacity_slots * page_size;
        if (ftruncate(fd, (off_t)size) != 0) {
            close(fd);
            return -1;
        }
    } else {
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            return -1;
        }
        size = (size_t)st.st_size;
    }

    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return -1;
    }

    pool->fd = fd;
    pool->base = base;
    pool->size = size;
    pool->page_size = page_size;
    return 0;
}

void dram_pool_close(dram_pool_t *pool, const char *name, int unlink_name) {
    if (!pool->base) return;
    munmap(pool->base, pool->size);
    close(pool->fd);
    if (unlink_name && name) shm_unlink(name);
    memset(pool, 0, sizeof(*pool));
}

void *dram_pool_slot(dram_pool_t *pool, int64_t slot_index) {
    if (slot_index < 0 || pool->page_size == 0) return NULL;
    size_t off = (size_t)slot_index * pool->page_size;
    if (off + pool->page_size > pool->size) return NULL;
    return (uint8_t *)pool->base + off;
}
