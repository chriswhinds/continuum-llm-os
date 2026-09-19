/* telemetry_store.h -- the in-memory table consoled aggregates node
 * health telemetry into. Written by the telemetry TCP listener thread
 * (one WIRE_HEALTH_REPORT per node-agentd health tick, ARCH-002 §05),
 * read by civetweb's HTTP handler thread(s) -- hence the mutex. */
#ifndef CONSOLED_TELEMETRY_STORE_H
#define CONSOLED_TELEMETRY_STORE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    float cpu_load_pct;
    uint64_t dram_used_bytes;
    uint64_t dram_total_bytes;
    uint64_t swap_io_bytes_per_sec;
    uint64_t last_seen_ns;
} node_stats_t;

typedef struct {
    uint32_t node_id;
    node_stats_t stats;
} node_stats_entry_t;

typedef struct telemetry_store telemetry_store_t;

telemetry_store_t *telemetry_store_create(size_t capacity);
void telemetry_store_destroy(telemetry_store_t *ts);

void telemetry_store_update(telemetry_store_t *ts, uint32_t node_id, const node_stats_t *stats);

/* Fills out (caller-allocated, max_out entries) with every known node's
 * latest stats. Returns the number written. */
size_t telemetry_store_snapshot(telemetry_store_t *ts, node_stats_entry_t *out, size_t max_out);

#endif
