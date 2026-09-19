#include "telemetry_store.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "hashtable.h"

struct telemetry_store {
    ht_t *ht;
    pthread_mutex_t lock;
};

telemetry_store_t *telemetry_store_create(size_t capacity) {
    telemetry_store_t *ts = calloc(1, sizeof(*ts));
    ts->ht = ht_create(capacity, sizeof(node_stats_t));
    pthread_mutex_init(&ts->lock, NULL);
    return ts;
}

void telemetry_store_destroy(telemetry_store_t *ts) {
    if (!ts) return;
    pthread_mutex_destroy(&ts->lock);
    ht_destroy(ts->ht);
    free(ts);
}

void telemetry_store_update(telemetry_store_t *ts, uint32_t node_id, const node_stats_t *stats) {
    pthread_mutex_lock(&ts->lock);
    ht_put(ts->ht, node_id, stats);
    pthread_mutex_unlock(&ts->lock);
}

typedef struct {
    node_stats_entry_t *out;
    size_t max;
    size_t count;
} snapshot_ctx_t;

static void snapshot_iter(uint64_t key, void *value, void *user) {
    snapshot_ctx_t *ctx = user;
    if (ctx->count >= ctx->max) return;
    ctx->out[ctx->count].node_id = (uint32_t)key;
    memcpy(&ctx->out[ctx->count].stats, value, sizeof(node_stats_t));
    ctx->count++;
}

size_t telemetry_store_snapshot(telemetry_store_t *ts, node_stats_entry_t *out, size_t max_out) {
    snapshot_ctx_t ctx = {.out = out, .max = max_out, .count = 0};
    pthread_mutex_lock(&ts->lock);
    ht_foreach(ts->ht, snapshot_iter, &ctx);
    pthread_mutex_unlock(&ts->lock);
    return ctx.count;
}
