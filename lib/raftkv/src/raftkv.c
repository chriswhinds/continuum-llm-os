#include "raftkv.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "hashtable.h"

#define OP_SET 1
#define OP_DEL 2

struct raftkv {
    raftnet_t *rn;
    ht_t *state;
    pthread_mutex_t lock;
    size_t value_size;
};

static int apply_cb(const void *data, size_t len, void *user) {
    raftkv_t *kv = user;
    if (len < 1 + 8) return 0;
    const uint8_t *p = data;
    uint8_t op = p[0];
    uint64_t key;
    memcpy(&key, p + 1, 8);

    pthread_mutex_lock(&kv->lock);
    if (op == OP_SET && len >= 1 + 8 + kv->value_size) {
        ht_put(kv->state, key, p + 9);
    } else if (op == OP_DEL) {
        ht_del(kv->state, key);
    }
    pthread_mutex_unlock(&kv->lock);
    return 0;
}

raftkv_t *raftkv_start(const raftnet_config_t *raft_cfg, size_t capacity, size_t value_size) {
    raftkv_t *kv = calloc(1, sizeof(*kv));
    kv->state = ht_create(capacity, value_size);
    kv->value_size = value_size;
    pthread_mutex_init(&kv->lock, NULL);

    raftnet_config_t cfg = *raft_cfg;
    cfg.apply = apply_cb;
    cfg.apply_user = kv;
    kv->rn = raftnet_start(&cfg);
    return kv;
}

void raftkv_stop(raftkv_t *kv) {
    if (!kv) return;
    raftnet_stop(kv->rn);
    pthread_mutex_destroy(&kv->lock);
    ht_destroy(kv->state);
    free(kv);
}

int raftkv_propose_set(raftkv_t *kv, uint64_t key, const void *value) {
    size_t len = 1 + 8 + kv->value_size;
    uint8_t *buf = malloc(len);
    buf[0] = OP_SET;
    memcpy(buf + 1, &key, 8);
    memcpy(buf + 9, value, kv->value_size);
    int rc = raftnet_propose(kv->rn, buf, len);
    free(buf);
    return rc;
}

int raftkv_propose_del(raftkv_t *kv, uint64_t key) {
    uint8_t buf[9];
    buf[0] = OP_DEL;
    memcpy(buf + 1, &key, 8);
    return raftnet_propose(kv->rn, buf, sizeof(buf));
}

int raftkv_get(raftkv_t *kv, uint64_t key, void *out) {
    pthread_mutex_lock(&kv->lock);
    void *v = ht_get(kv->state, key);
    int found = v != NULL;
    if (found && out) memcpy(out, v, kv->value_size);
    pthread_mutex_unlock(&kv->lock);
    return found;
}

int raftkv_is_leader(const raftkv_t *kv) { return raftnet_is_leader(kv->rn); }
int raftkv_leader_node_id(const raftkv_t *kv) { return raftnet_leader_node_id(kv->rn); }

size_t raftkv_count(raftkv_t *kv) {
    pthread_mutex_lock(&kv->lock);
    size_t c = ht_count(kv->state);
    pthread_mutex_unlock(&kv->lock);
    return c;
}
