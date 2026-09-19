/* raftnet.h -- glues the vendored willemt/raft consensus core to Continuum's
 * TCP wire protocol, for page-directoryd and membershipd (ARCH-002 §05).
 *
 * Each raftnet_t owns a small dedicated networking thread that: accepts/
 * maintains one TCP connection per peer, ticks raft_periodic() on a timer,
 * and feeds inbound RAFT_* wire frames into the raft core. The owning
 * daemon's main thread never touches raft_server_t directly -- it calls
 * raftnet_propose() to write and gets committed entries back through the
 * apply callback (invoked on the networking thread, so the callback must
 * do its own locking around whatever state it updates).
 *
 * Scope note: this reference implementation keeps the raft log purely
 * in-memory -- persist_vote/persist_term/log_offer/poll/pop are no-ops.
 * A node that crashes loses its vote/term state and rejoins by catching up
 * from the current leader's log, which is safe for Raft's correctness but
 * means a full quorum restart (all 3 nodes down at once) loses committed
 * history. Durable per-node persistence to NVMe is the obvious next step
 * and is called out here rather than silently skipped.
 */
#ifndef CONTINUUM_RAFTNET_H
#define CONTINUUM_RAFTNET_H

#include <stddef.h>
#include <stdint.h>

typedef struct raftnet raftnet_t;

typedef struct {
    int node_id;
    char host[64];
    uint16_t port;
} raftnet_peer_t;

/* Invoked once per committed normal log entry, in log order, on raftnet's
 * internal thread. Must return 0; the return value is reserved for future
 * use (e.g. triggering a snapshot) and is currently ignored by raft.c. */
typedef int (*raftnet_apply_fn)(const void *data, size_t len, void *user);

typedef struct {
    int my_node_id;
    const raftnet_peer_t *peers; /* must include an entry for my_node_id */
    size_t n_peers;
    raftnet_apply_fn apply;
    void *apply_user;
    int election_timeout_ms;  /* 0 => default 1000 */
    int request_timeout_ms;   /* 0 => default 200 */
    const char *log_tag;      /* for clog, e.g. "raftnet/pagedir" */
} raftnet_config_t;

/* Starts the networking thread and returns immediately; the cluster forms
 * in the background as connections come up and elections run. */
raftnet_t *raftnet_start(const raftnet_config_t *cfg);

/* Stops the thread and frees everything. Blocks until the thread exits. */
void raftnet_stop(raftnet_t *rn);

/* Proposes data as a new FSM command. Returns 0 if accepted for
 * replication (NOT yet committed -- the apply callback fires later, once a
 * majority has it), -1 if this node isn't the current leader. */
int raftnet_propose(raftnet_t *rn, const void *data, size_t len);

int raftnet_is_leader(const raftnet_t *rn);
/* -1 if no leader is currently known. */
int raftnet_leader_node_id(const raftnet_t *rn);

#endif
