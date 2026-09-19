#include "telemetry_store.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    telemetry_store_t *ts = telemetry_store_create(64);

    node_stats_t a = {.cpu_load_pct = 42.5f, .dram_used_bytes = 1000, .dram_total_bytes = 2000, .swap_io_bytes_per_sec = 50, .last_seen_ns = 111};
    node_stats_t b = {.cpu_load_pct = 10.0f, .dram_used_bytes = 500, .dram_total_bytes = 1000, .swap_io_bytes_per_sec = 0, .last_seen_ns = 222};
    telemetry_store_update(ts, 1, &a);
    telemetry_store_update(ts, 2, &b);

    node_stats_entry_t out[16];
    size_t n = telemetry_store_snapshot(ts, out, 16);
    assert(n == 2);
    printf("ok: two distinct nodes tracked\n");

    /* A later update for node 1 must replace, not duplicate. */
    node_stats_t a2 = {.cpu_load_pct = 99.0f, .dram_used_bytes = 1900, .dram_total_bytes = 2000, .swap_io_bytes_per_sec = 500, .last_seen_ns = 333};
    telemetry_store_update(ts, 1, &a2);
    n = telemetry_store_snapshot(ts, out, 16);
    assert(n == 2);
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (out[i].node_id == 1) {
            assert(out[i].stats.cpu_load_pct == 99.0f);
            found = 1;
        }
    }
    assert(found);
    printf("ok: a repeat update replaces the node's stats rather than duplicating it\n");

    telemetry_store_destroy(ts);
    printf("all telemetry_store tests passed\n");
    return 0;
}
