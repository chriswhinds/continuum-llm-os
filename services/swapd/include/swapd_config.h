/* swapd_config.h -- config file format for swapd.
 *
 * node_id 7
 * weight_file /var/lib/continuum/shard-07.weights
 * tensor_region_bytes 268435456
 * page_size 2097152
 * unix_socket /run/continuum/swapd.sock
 * remote_peer 10.0.0.12:7301
 */
#ifndef SWAPD_CONFIG_H
#define SWAPD_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define SWAPD_MAX_PEERS 8

typedef struct {
    char host[64];
    uint16_t port;
} swapd_peer_t;

typedef struct {
    uint32_t node_id;
    char weight_file[256];
    uint64_t tensor_region_bytes;
    uint64_t page_size;
    /* How many pages may be DRAM-resident at once -- the node's actual RAM
     * budget for this shard, independent of and normally smaller than
     * tensor_region_bytes/page_size (the full addressable tensor size).
     * Defaults to covering the whole tensor region (no eviction pressure)
     * if left at 0/unset, which is fine for small demo shards but not the
     * point of the exercise -- set it explicitly to see swap in action. */
    uint64_t dram_capacity_pages;
    char unix_socket[128];
    swapd_peer_t peers[SWAPD_MAX_PEERS];
    size_t n_peers;
    /* high_watermark_pct: once resident pages exceed this fraction of the
     * table's capacity, the eviction sweep starts reclaiming the least-
     * recently-touched pages (ARCH-002 §05's LFU+recency policy,
     * simplified here to pure LRU -- see swapd's module comment). */
    int high_watermark_pct;
} swapd_config_t;

int swapd_config_load(const char *path, swapd_config_t *out);

#endif
