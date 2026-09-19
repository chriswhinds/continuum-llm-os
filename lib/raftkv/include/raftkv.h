/* raftkv.h -- a small replicated key-value store built on lib/raftnet.
 * page-directoryd and membershipd are both, structurally, "a Raft group
 * whose FSM is a key-value table" (ARCH-002 §05) -- the page directory
 * keyed by page id, membership keyed by node id. This is that common
 * piece, factored out once instead of duplicated twice.
 *
 * Reads (raftkv_get) are served from this node's own locally-applied
 * state, not forwarded to the leader -- so a follower can answer a read
 * microseconds after a write commits elsewhere but strictly consistent
 * linearizable reads aren't guaranteed (a follower can lag). ARCH-001's
 * directory and membership data change rarely enough, relative to how
 * often they're read, that eventual consistency here is a reasonable
 * trade -- the alternative (proxying every read through the leader) would
 * turn the control plane into a bottleneck for exactly the kind of
 * lookup that should be cheap.
 */
#ifndef CONTINUUM_RAFTKV_H
#define CONTINUUM_RAFTKV_H

#include <stddef.h>
#include <stdint.h>

#include "raftnet.h"

typedef struct raftkv raftkv_t;

/* value_size is fixed for the table's lifetime (like lib/pagetable's
 * ht_t, which this is built on internally). capacity is the maximum
 * number of distinct keys. */
raftkv_t *raftkv_start(const raftnet_config_t *raft_cfg, size_t capacity, size_t value_size);
void raftkv_stop(raftkv_t *kv);

/* Proposes a write; returns 0 if accepted for replication (not yet
 * necessarily committed/applied -- see raftkv_get), -1 if this node isn't
 * the current leader (caller should retry against raftkv_leader_node_id()). */
int raftkv_propose_set(raftkv_t *kv, uint64_t key, const void *value);
int raftkv_propose_del(raftkv_t *kv, uint64_t key);

/* Local read, per the module comment above. Returns 1 and fills *out if
 * present, 0 if not (either genuinely absent, or not yet applied here). */
int raftkv_get(raftkv_t *kv, uint64_t key, void *out);

int raftkv_is_leader(const raftkv_t *kv);
int raftkv_leader_node_id(const raftkv_t *kv);
size_t raftkv_count(raftkv_t *kv);

#endif
