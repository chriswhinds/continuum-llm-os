#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "raftnet.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "clog.h"
#include "raft.h"
#include "sock_util.h"
#include "wire.h"

#define RAFTNET_TICK_MS 100
#define RAFTNET_HANDSHAKE_TIMEOUT_SEC 2

typedef struct {
    int node_id;
    char host[64];
    uint16_t port;
    int fd;             /* -1 if not connected */
    wire_conn_t *conn;   /* NULL if not connected */
    raft_node_t *raft_node;
    int is_self;
} peer_slot_t;

struct raftnet {
    raft_server_t *raft;
    pthread_mutex_t lock;
    pthread_t thread;
    volatile int stop_flag;
    int epfd;
    int timerfd;
    int listen_fd;
    peer_slot_t *peers;
    size_t n_peers;
    size_t my_index;
    raftnet_apply_fn apply;
    void *apply_user;
    char log_tag[32];
    /* Sentinels distinguishing epoll_event.data.ptr for the listen socket
     * and the timer, since peer connections use &peers[i] there instead. */
    int listen_marker;
    int timer_marker;
};

/* dispatch context handed to wire_conn_on_readable()'s callback, so it can
 * reach both the raftnet instance and which peer this connection is. */
typedef struct {
    raftnet_t *rn;
    peer_slot_t *peer;
} dispatch_ctx_t;

/* ---------------------------------------------------------------------
 * Entry (de)serialization.
 *
 * REQUESTVOTE_{REQ,RESP} and APPENDENTRIES_RESP are POD structs (no
 * pointers) so they go on the wire as a raw memcpy. APPENDENTRIES_REQ
 * carries a variable-length array of log entries, each with its own
 * variable-length data blob, so it needs real packing.
 *
 * Ownership: raft's internal log (log_append_entry(), raft_log.c) does a
 * *shallow* memcpy of each raft_entry_t it stores -- it copies the
 * data.buf pointer, not the bytes behind it. So every entry data buffer
 * unpacked off the wire (or allocated by raftnet_propose() below) is
 * handed to raft and then deliberately never freed by this code: it lives
 * for the process's lifetime, because this reference build never persists
 * or compacts the log (see the header comment). That's fine for a
 * directory/membership workload's data volume in a demo/reference
 * cluster; a production build would add snapshotting before this became a
 * real leak.
 * ------------------------------------------------------------------- */

static uint8_t *pack_appendentries(const msg_appendentries_t *ae, uint32_t *out_len) {
    size_t size = 8 + 8 + 8 + 8 + 4; /* term, prev_log_idx, prev_log_term, leader_commit, n_entries */
    for (int i = 0; i < ae->n_entries; i++) {
        size += 4 + 8 + 4 + 4 + ae->entries[i].data.len; /* id, term, type, data_len, data */
    }
    uint8_t *buf = malloc(size);
    if (!buf) return NULL;

    uint8_t *p = buf;
    int64_t term = ae->term, prev_idx = ae->prev_log_idx, prev_term = ae->prev_log_term, commit = ae->leader_commit;
    int32_t n = ae->n_entries;
    memcpy(p, &term, 8); p += 8;
    memcpy(p, &prev_idx, 8); p += 8;
    memcpy(p, &prev_term, 8); p += 8;
    memcpy(p, &commit, 8); p += 8;
    memcpy(p, &n, 4); p += 4;

    for (int i = 0; i < ae->n_entries; i++) {
        int32_t id = ae->entries[i].id;
        int64_t etterm = ae->entries[i].term;
        int32_t type = ae->entries[i].type;
        uint32_t dlen = ae->entries[i].data.len;
        memcpy(p, &id, 4); p += 4;
        memcpy(p, &etterm, 8); p += 8;
        memcpy(p, &type, 4); p += 4;
        memcpy(p, &dlen, 4); p += 4;
        if (dlen) {
            memcpy(p, ae->entries[i].data.buf, dlen);
            p += dlen;
        }
    }
    *out_len = (uint32_t)size;
    return buf;
}

/* On success, fills *out (out->entries malloc'd -- caller frees the array
 * with free(out->entries) once done, but must NOT free entries[i].data.buf,
 * per the ownership note above). Returns 0 on success, -1 on a malformed
 * frame (short/corrupt). */
static int unpack_appendentries(const uint8_t *buf, uint32_t len, msg_appendentries_t *out) {
    memset(out, 0, sizeof(*out));
    if (len < 8 + 8 + 8 + 8 + 4) return -1;
    const uint8_t *p = buf;
    int64_t term, prev_idx, prev_term, commit;
    int32_t n;
    memcpy(&term, p, 8); p += 8;
    memcpy(&prev_idx, p, 8); p += 8;
    memcpy(&prev_term, p, 8); p += 8;
    memcpy(&commit, p, 8); p += 8;
    memcpy(&n, p, 4); p += 4;
    if (n < 0) return -1;

    msg_entry_t *entries = n ? calloc((size_t)n, sizeof(msg_entry_t)) : NULL;
    if (n && !entries) return -1;

    for (int32_t i = 0; i < n; i++) {
        if ((size_t)(p - buf) + 4 + 8 + 4 + 4 > len) { free(entries); return -1; }
        int32_t id, type;
        int64_t etterm;
        uint32_t dlen;
        memcpy(&id, p, 4); p += 4;
        memcpy(&etterm, p, 8); p += 8;
        memcpy(&type, p, 4); p += 4;
        memcpy(&dlen, p, 4); p += 4;
        if ((size_t)(p - buf) + dlen > len) { free(entries); return -1; }

        entries[i].id = id;
        entries[i].term = etterm;
        entries[i].type = type;
        entries[i].data.len = dlen;
        if (dlen) {
            entries[i].data.buf = malloc(dlen);
            if (!entries[i].data.buf) { free(entries); return -1; }
            memcpy(entries[i].data.buf, p, dlen);
            p += dlen;
        }
    }

    out->term = term;
    out->prev_log_idx = prev_idx;
    out->prev_log_term = prev_term;
    out->leader_commit = commit;
    out->n_entries = n;
    out->entries = entries;
    return 0;
}

/* ---------------------------------------------------------------------
 * raft_cbs_t callbacks
 * ------------------------------------------------------------------- */

static int cb_send_requestvote(raft_server_t *raft, void *udata, raft_node_t *node, msg_requestvote_t *msg) {
    (void)raft;
    (void)udata;
    peer_slot_t *peer = raft_node_get_udata(node);
    if (!peer || !peer->conn) return 0; /* not connected -- raft will retry on the next election timeout */
    int rc = wire_conn_enqueue(peer->conn, WIRE_RAFT_REQUESTVOTE_REQ, 0, 0, msg, sizeof(*msg));
    if (rc == 0) wire_conn_flush(peer->conn);
    return rc;
}

static int cb_send_appendentries(raft_server_t *raft, void *udata, raft_node_t *node, msg_appendentries_t *msg) {
    (void)raft;
    (void)udata;
    peer_slot_t *peer = raft_node_get_udata(node);
    if (!peer || !peer->conn) return 0;
    uint32_t len = 0;
    uint8_t *buf = pack_appendentries(msg, &len);
    if (!buf) return -1;
    int rc = wire_conn_enqueue(peer->conn, WIRE_RAFT_APPENDENTRIES_REQ, 0, 0, buf, len);
    if (rc == 0) wire_conn_flush(peer->conn);
    free(buf);
    return rc;
}

static int cb_send_snapshot(raft_server_t *raft, void *udata, raft_node_t *node) {
    (void)raft; (void)udata; (void)node;
    /* No snapshotting in this reference build (see raftnet.h). We never call
     * raft_begin_snapshot(), so this should not fire in practice. */
    clog_warn("raftnet: send_snapshot requested but snapshotting isn't implemented");
    return 0;
}

static int cb_applylog(raft_server_t *raft, void *udata, raft_entry_t *entry, raft_index_t idx) {
    (void)raft;
    (void)idx;
    raftnet_t *rn = udata;
    if (entry->type != RAFT_LOGTYPE_NORMAL) return 0; /* config-change entries: nothing for the app to apply */
    if (rn->apply) rn->apply(entry->data.buf, entry->data.len, rn->apply_user);
    return 0;
}

/* No durable persistence in this reference build -- see raftnet.h. */
static int cb_noop_persist(raft_server_t *raft, void *udata, raft_node_id_t vote) { (void)raft; (void)udata; (void)vote; return 0; }
static int cb_noop_persist_term(raft_server_t *raft, void *udata, raft_term_t term, raft_node_id_t vote) { (void)raft; (void)udata; (void)term; (void)vote; return 0; }
static int cb_noop_log_event(raft_server_t *raft, void *udata, raft_entry_t *entry, raft_index_t idx) { (void)raft; (void)udata; (void)entry; (void)idx; return 0; }

static void cb_log(raft_server_t *raft, raft_node_t *node, void *udata, const char *buf) {
    (void)raft; (void)node;
    raftnet_t *rn = udata;
    clog_debug("%s: %s", rn->log_tag, buf);
}

/* ---------------------------------------------------------------------
 * Connection setup
 * ------------------------------------------------------------------- */

static peer_slot_t *find_peer_by_id(raftnet_t *rn, int node_id) {
    for (size_t i = 0; i < rn->n_peers; i++) {
        if (rn->peers[i].node_id == node_id) return &rn->peers[i];
    }
    return NULL;
}

static void epoll_add(int epfd, int fd, uint32_t events, void *ptr) {
    struct epoll_event ev = {.events = events, .data = {.ptr = ptr}};
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}
static void epoll_mod(int epfd, int fd, uint32_t events, void *ptr) {
    struct epoll_event ev = {.events = events, .data = {.ptr = ptr}};
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

static void attach_peer_conn(raftnet_t *rn, peer_slot_t *peer, int fd) {
    peer->fd = fd;
    peer->conn = wire_conn_new(fd);
    sock_set_nonblocking(fd);
    epoll_add(rn->epfd, fd, EPOLLIN, peer);
}

static void detach_peer_conn(raftnet_t *rn, peer_slot_t *peer) {
    if (peer->fd >= 0) {
        epoll_ctl(rn->epfd, EPOLL_CTL_DEL, peer->fd, NULL);
        close(peer->fd);
    }
    if (peer->conn) wire_conn_free(peer->conn);
    peer->conn = NULL;
    peer->fd = -1;
}

/* Blocking handshake with a short timeout, run on a freshly connected or
 * accepted fd before it joins the non-blocking epoll set. See raftnet.h /
 * the module comment for why this reference build accepts a brief stall
 * here rather than implementing a fully async handshake. */
static int do_handshake(int fd, int my_node_id, int *out_peer_node_id) {
    struct timeval tv = {.tv_sec = RAFTNET_HANDSHAKE_TIMEOUT_SEC, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int32_t my_id_le = my_node_id;
    if (wire_send_frame_blocking(fd, WIRE_RAFT_HELLO, 0, 0, &my_id_le, sizeof(my_id_le)) != 0) return -1;

    uint8_t type, flags;
    uint64_t stream_id;
    uint8_t buf[16];
    uint32_t paylen;
    if (wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) != 0) return -1;
    if (type != WIRE_RAFT_HELLO || paylen != 4) return -1;

    int32_t peer_id;
    memcpy(&peer_id, buf, 4);
    *out_peer_node_id = peer_id;

    tv.tv_sec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return 0;
}

static void try_accept(raftnet_t *rn) {
    for (;;) {
        int fd = accept(rn->listen_fd, NULL, NULL);
        if (fd < 0) return;

        int peer_id = -1;
        if (do_handshake(fd, rn->peers[rn->my_index].node_id, &peer_id) != 0) {
            clog_warn("%s: handshake failed on inbound connection, dropping", rn->log_tag);
            close(fd);
            continue;
        }
        peer_slot_t *peer = find_peer_by_id(rn, peer_id);
        if (!peer || peer->is_self) {
            clog_warn("%s: inbound connection claimed unknown node_id=%d", rn->log_tag, peer_id);
            close(fd);
            continue;
        }
        if (peer->conn) detach_peer_conn(rn, peer); /* replace a stale/duplicate connection */
        attach_peer_conn(rn, peer, fd);
        clog_info("%s: accepted connection from node %d", rn->log_tag, peer_id);
    }
}

/* Only the numerically-lower node_id initiates, so a pair of peers never
 * race to open two redundant connections between them. */
static void try_connect_outbound(raftnet_t *rn) {
    int my_id = rn->peers[rn->my_index].node_id;
    for (size_t i = 0; i < rn->n_peers; i++) {
        peer_slot_t *peer = &rn->peers[i];
        if (peer->is_self || peer->conn) continue;
        if (my_id >= peer->node_id) continue; /* the higher-id peer initiates instead */

        int fd = sock_tcp_connect(peer->host, peer->port);
        if (fd < 0) continue; /* peer not up yet; retry next tick */

        int confirmed_id = -1;
        if (do_handshake(fd, my_id, &confirmed_id) != 0 || confirmed_id != peer->node_id) {
            close(fd);
            continue;
        }
        attach_peer_conn(rn, peer, fd);
        clog_info("%s: connected to node %d", rn->log_tag, peer->node_id);
    }
}

/* ---------------------------------------------------------------------
 * Frame dispatch
 * ------------------------------------------------------------------- */

static void handle_frame(void *user, uint8_t type, uint8_t flags, uint64_t stream_id,
                          const uint8_t *payload, uint32_t paylen) {
    (void)flags;
    (void)stream_id;
    dispatch_ctx_t *ctx = user;
    raftnet_t *rn = ctx->rn;
    peer_slot_t *peer = ctx->peer;

    switch (type) {
        case WIRE_RAFT_REQUESTVOTE_REQ: {
            if (paylen != sizeof(msg_requestvote_t)) return;
            msg_requestvote_t msg;
            memcpy(&msg, payload, sizeof(msg));
            msg_requestvote_response_t resp;
            raft_recv_requestvote(rn->raft, peer->raft_node, &msg, &resp);
            wire_conn_enqueue(peer->conn, WIRE_RAFT_REQUESTVOTE_RESP, 0, 0, &resp, sizeof(resp));
            wire_conn_flush(peer->conn);
            break;
        }
        case WIRE_RAFT_REQUESTVOTE_RESP: {
            if (paylen != sizeof(msg_requestvote_response_t)) return;
            msg_requestvote_response_t resp;
            memcpy(&resp, payload, sizeof(resp));
            raft_recv_requestvote_response(rn->raft, peer->raft_node, &resp);
            break;
        }
        case WIRE_RAFT_APPENDENTRIES_REQ: {
            msg_appendentries_t msg;
            if (unpack_appendentries(payload, paylen, &msg) != 0) {
                clog_warn("%s: malformed appendentries from node %d", rn->log_tag, peer->node_id);
                return;
            }
            msg_appendentries_response_t resp;
            raft_recv_appendentries(rn->raft, peer->raft_node, &msg, &resp);
            free(msg.entries); /* array only -- entry data.buf now owned by raft's log, see header note */
            wire_conn_enqueue(peer->conn, WIRE_RAFT_APPENDENTRIES_RESP, 0, 0, &resp, sizeof(resp));
            wire_conn_flush(peer->conn);
            break;
        }
        case WIRE_RAFT_APPENDENTRIES_RESP: {
            if (paylen != sizeof(msg_appendentries_response_t)) return;
            msg_appendentries_response_t resp;
            memcpy(&resp, payload, sizeof(resp));
            raft_recv_appendentries_response(rn->raft, peer->raft_node, &resp);
            break;
        }
        default:
            clog_debug("%s: ignoring unexpected wire type %u on raft connection", rn->log_tag, type);
    }
}

/* ---------------------------------------------------------------------
 * Thread main
 * ------------------------------------------------------------------- */

static void *raftnet_thread_main(void *arg) {
    raftnet_t *rn = arg;
    struct epoll_event events[16];

    while (!rn->stop_flag) {
        int n = epoll_wait(rn->epfd, events, 16, 200);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        pthread_mutex_lock(&rn->lock);
        for (int i = 0; i < n; i++) {
            void *ptr = events[i].data.ptr;
            if (ptr == &rn->listen_marker) {
                try_accept(rn);
            } else if (ptr == &rn->timer_marker) {
                uint64_t expirations;
                ssize_t r = read(rn->timerfd, &expirations, sizeof(expirations));
                (void)r;
                raft_periodic(rn->raft, RAFTNET_TICK_MS);
                try_connect_outbound(rn);
            } else {
                peer_slot_t *peer = ptr;
                dispatch_ctx_t ctx = {.rn = rn, .peer = peer};
                if (events[i].events & EPOLLOUT) {
                    if (peer->conn && wire_conn_flush(peer->conn) == 0) {
                        epoll_mod(rn->epfd, peer->fd, EPOLLIN, peer);
                    }
                }
                if (peer->conn && (events[i].events & EPOLLIN)) {
                    if (wire_conn_on_readable(peer->conn, handle_frame, &ctx) != 0) {
                        clog_info("%s: connection to node %d dropped", rn->log_tag, peer->node_id);
                        detach_peer_conn(rn, peer);
                    } else if (peer->conn && wire_conn_has_pending_writes(peer->conn)) {
                        epoll_mod(rn->epfd, peer->fd, EPOLLIN | EPOLLOUT, peer);
                    }
                }
            }
        }
        pthread_mutex_unlock(&rn->lock);
    }
    return NULL;
}

/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */

raftnet_t *raftnet_start(const raftnet_config_t *cfg) {
    raftnet_t *rn = calloc(1, sizeof(*rn));
    if (!rn) return NULL;

    pthread_mutex_init(&rn->lock, NULL);
    strncpy(rn->log_tag, cfg->log_tag ? cfg->log_tag : "raftnet", sizeof(rn->log_tag) - 1);

    rn->n_peers = cfg->n_peers;
    rn->peers = calloc(cfg->n_peers, sizeof(peer_slot_t));
    rn->apply = cfg->apply;
    rn->apply_user = cfg->apply_user;

    uint16_t my_port = 0;
    for (size_t i = 0; i < cfg->n_peers; i++) {
        rn->peers[i].node_id = cfg->peers[i].node_id;
        strncpy(rn->peers[i].host, cfg->peers[i].host, sizeof(rn->peers[i].host) - 1);
        rn->peers[i].port = cfg->peers[i].port;
        rn->peers[i].fd = -1;
        rn->peers[i].is_self = (cfg->peers[i].node_id == cfg->my_node_id);
        if (rn->peers[i].is_self) {
            rn->my_index = i;
            my_port = cfg->peers[i].port;
        }
    }

    rn->raft = raft_new();
    raft_cbs_t cbs = {
        .send_requestvote = cb_send_requestvote,
        .send_appendentries = cb_send_appendentries,
        .send_snapshot = cb_send_snapshot,
        .applylog = cb_applylog,
        .persist_vote = cb_noop_persist,
        .persist_term = cb_noop_persist_term,
        .log_offer = cb_noop_log_event,
        .log_poll = cb_noop_log_event,
        .log_pop = cb_noop_log_event,
        .log_clear = cb_noop_log_event,
        .log_get_node_id = NULL,
        .node_has_sufficient_logs = NULL,
        .notify_membership_event = NULL,
        .log = cb_log,
    };
    raft_set_callbacks(rn->raft, &cbs, rn);
    if (cfg->election_timeout_ms > 0) raft_set_election_timeout(rn->raft, cfg->election_timeout_ms);
    if (cfg->request_timeout_ms > 0) raft_set_request_timeout(rn->raft, cfg->request_timeout_ms);

    for (size_t i = 0; i < cfg->n_peers; i++) {
        raft_node_t *node = raft_add_node(rn->raft, &rn->peers[i], rn->peers[i].node_id, rn->peers[i].is_self);
        rn->peers[i].raft_node = node;
    }

    rn->epfd = epoll_create1(0);
    rn->listen_fd = sock_tcp_listen(NULL, my_port, 16);
    if (rn->listen_fd < 0) {
        clog_error("%s: failed to listen on port %u", rn->log_tag, my_port);
    } else {
        epoll_add(rn->epfd, rn->listen_fd, EPOLLIN, &rn->listen_marker);
    }

    rn->timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = {
        .it_interval = {.tv_sec = 0, .tv_nsec = RAFTNET_TICK_MS * 1000000L},
        .it_value = {.tv_sec = 0, .tv_nsec = RAFTNET_TICK_MS * 1000000L},
    };
    timerfd_settime(rn->timerfd, 0, &its, NULL);
    epoll_add(rn->epfd, rn->timerfd, EPOLLIN, &rn->timer_marker);

    pthread_create(&rn->thread, NULL, raftnet_thread_main, rn);
    return rn;
}

void raftnet_stop(raftnet_t *rn) {
    if (!rn) return;
    rn->stop_flag = 1;
    pthread_join(rn->thread, NULL);

    for (size_t i = 0; i < rn->n_peers; i++) {
        if (rn->peers[i].conn) detach_peer_conn(rn, &rn->peers[i]);
    }
    if (rn->listen_fd >= 0) close(rn->listen_fd);
    close(rn->timerfd);
    close(rn->epfd);
    raft_free(rn->raft);
    pthread_mutex_destroy(&rn->lock);
    free(rn->peers);
    free(rn);
}

int raftnet_propose(raftnet_t *rn, const void *data, size_t len) {
    pthread_mutex_lock(&rn->lock);
    if (!raft_is_leader(rn->raft)) {
        pthread_mutex_unlock(&rn->lock);
        return -1;
    }
    void *copy = malloc(len); /* ownership transfers to raft's log, see the ownership note above */
    if (len && !copy) {
        pthread_mutex_unlock(&rn->lock);
        return -1;
    }
    if (len) memcpy(copy, data, len);

    msg_entry_t entry = {0};
    entry.term = raft_get_current_term(rn->raft);
    entry.id = (raft_entry_id_t)rand();
    entry.type = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = copy;
    entry.data.len = (unsigned int)len;

    msg_entry_response_t resp;
    int rc = raft_recv_entry(rn->raft, &entry, &resp);
    pthread_mutex_unlock(&rn->lock);
    return rc == 0 ? 0 : -1;
}

int raftnet_is_leader(const raftnet_t *rn) {
    raftnet_t *mrn = (raftnet_t *)rn;
    pthread_mutex_lock(&mrn->lock);
    int r = raft_is_leader(mrn->raft);
    pthread_mutex_unlock(&mrn->lock);
    return r;
}

int raftnet_leader_node_id(const raftnet_t *rn) {
    raftnet_t *mrn = (raftnet_t *)rn;
    pthread_mutex_lock(&mrn->lock);
    raft_node_t *leader = raft_get_current_leader_node(mrn->raft);
    int id = -1;
    if (leader) {
        peer_slot_t *peer = raft_node_get_udata(leader);
        if (peer) id = peer->node_id;
    }
    pthread_mutex_unlock(&mrn->lock);
    return id;
}
