/* wire.h -- Continuum's node-to-node wire protocol (ARCH-001 §07, ARCH-002 §05).
 *
 * A frame is a fixed 16-byte header followed by `length` bytes of payload:
 *
 *     u32 length | u8 type | u8 flags | u16 reserved | u64 stream_id | payload
 *
 * Multi-byte header fields are written host-endian with a plain memcpy, not
 * converted to network byte order. Every board in ARCH-002's reference
 * hardware -- the x86_64 dev machine and the aarch64 Raspberry Pi 5 fleet --
 * is little-endian, so this is a deliberate simplification, not an
 * oversight: it avoids a byte-swap on every page transfer on the only
 * platforms this build targets. Porting to a big-endian node would mean
 * adding the swap back in wire_conn_enqueue()/wire_conn_on_readable().
 *
 * This header also declares a small non-blocking, buffered connection type
 * (wire_conn_t) used by every epoll-driven daemon in the tree, plus a pair
 * of blocking helpers for simple synchronous callers (CLI tools, tests).
 */
#ifndef CONTINUUM_WIRE_H
#define CONTINUUM_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define WIRE_HEADER_SIZE 16u
#define WIRE_MAX_PAYLOAD (16u * 1024u * 1024u) /* 16MB -- generous for one tensor page */

typedef enum {
    WIRE_HEARTBEAT              = 1,  /* node-agentd/continuumd -> control plane, empty payload */
    WIRE_PAGE_FETCH_REQ         = 2,  /* wire_page_fetch_req_t */
    WIRE_PAGE_FETCH_RESP        = 3,  /* wire_page_fetch_resp_t header + raw tensor bytes */
    WIRE_ACTIVATION_HOP         = 4,  /* pipeline-stage activation hand-off, raw bytes */
    WIRE_TOKEN_CHUNK            = 5,  /* one decode-step token, scheduler <-> gateway */
    WIRE_DIRECTORY_QUERY        = 6,  /* wire_directory_query_t */
    WIRE_DIRECTORY_RESP         = 7,  /* wire_directory_resp_t */
    WIRE_HEALTH_REPORT          = 8,  /* wire_health_report_t */
    WIRE_DISPATCH_PROMPT        = 9,  /* scheduler -> node-agentd/shard-execd, prompt tokens */
    WIRE_SCHED_REQUEST          = 10, /* api-gatewayd -> schedulerd, JSON request body */
    WIRE_SCHED_RESPONSE         = 11, /* schedulerd -> api-gatewayd, JSON or error */
    WIRE_RAFT_REQUESTVOTE_REQ   = 20,
    WIRE_RAFT_REQUESTVOTE_RESP  = 21,
    WIRE_RAFT_APPENDENTRIES_REQ = 22,
    WIRE_RAFT_APPENDENTRIES_RESP= 23,
    WIRE_RAFT_HELLO              = 24, /* connection handshake: payload is a little-endian int32 node_id */
    WIRE_ERROR                  = 255,
} wire_msg_type_t;

/* Set on WIRE_TOKEN_CHUNK / WIRE_SCHED_RESPONSE frames that are the last one
 * in a stream, so the receiver knows to stop without a separate "done"
 * message. */
#define WIRE_FLAG_FINAL 0x01u

#pragma pack(push, 1)
typedef struct {
    uint32_t length;
    uint8_t  type;
    uint8_t  flags;
    uint16_t reserved;
    uint64_t stream_id;
} wire_header_t;

/* Page tier, mirrors ARCH-001 §03's five-tier address space (tier 0 is an
 * optional GPU/accelerator tier this reference build does not use). */
typedef enum {
    WIRE_TIER_LOCAL_DRAM  = 1,
    WIRE_TIER_REMOTE_DRAM = 2,
    WIRE_TIER_LOCAL_NVME  = 3,
    WIRE_TIER_REMOTE_NVME = 4,
} wire_tier_t;

typedef struct {
    uint64_t page_id;
} wire_page_fetch_req_t;

typedef struct {
    uint64_t page_id;
    uint8_t  tier;      /* wire_tier_t */
    uint8_t  dtype;      /* caller-defined tensor element type tag */
    uint16_t reserved;
    uint32_t shape[4];
    uint32_t byte_len;   /* length of the tensor bytes following this header */
} wire_page_fetch_resp_t;

typedef struct {
    uint64_t page_id;
} wire_directory_query_t;

typedef struct {
    uint64_t page_id;
    uint32_t owner_node_id;
    uint8_t  tier;
    uint8_t  found; /* 0 if the page is unknown to the directory */
    uint16_t reserved;
} wire_directory_resp_t;

typedef struct {
    uint32_t node_id;
    float    cpu_load_pct;
    uint64_t dram_used_bytes;
    uint64_t dram_total_bytes;
    uint64_t swap_io_bytes_per_sec;
} wire_health_report_t;
#pragma pack(pop)

/* ---- non-blocking, buffered connection ---------------------------------- */

typedef struct wire_conn wire_conn_t;

wire_conn_t *wire_conn_new(int fd);
void wire_conn_free(wire_conn_t *c);
int wire_conn_fd(const wire_conn_t *c);

/* Copies header+payload into the connection's send buffer. Returns 0 on
 * success, -1 if paylen exceeds WIRE_MAX_PAYLOAD. Does not perform I/O --
 * call wire_conn_flush() (typically from an EPOLLOUT handler) to send it. */
int wire_conn_enqueue(wire_conn_t *c, uint8_t type, uint8_t flags,
                       uint64_t stream_id, const void *payload, uint32_t paylen);

/* Writes as much of the pending send buffer as the socket accepts right
 * now. Returns 0 if the buffer was fully flushed, 1 if data remains queued
 * (caller should keep EPOLLOUT armed), -1 on a hard error (peer gone). */
int wire_conn_flush(wire_conn_t *c);
int wire_conn_has_pending_writes(const wire_conn_t *c);

typedef void (*wire_frame_cb)(void *user, uint8_t type, uint8_t flags,
                               uint64_t stream_id, const uint8_t *payload, uint32_t paylen);

/* Reads whatever is available on the fd and invokes cb() once per complete
 * frame found (a single readable event may yield zero, one, or many
 * frames). Returns 0 on success, -1 if the peer closed or a hard error
 * occurred (errno set); EAGAIN/EWOULDBLOCK after reading nothing is not an
 * error and is folded into a 0 return. */
int wire_conn_on_readable(wire_conn_t *c, wire_frame_cb cb, void *user);

/* ---- blocking helpers for simple synchronous callers --------------------- */

int wire_send_frame_blocking(int fd, uint8_t type, uint8_t flags,
                              uint64_t stream_id, const void *payload, uint32_t paylen);

/* On success, *paylen is set to the number of payload bytes copied into buf
 * (buf must be at least bufcap bytes). Returns 0 on success, -1 on error or
 * peer close, -2 if the frame's payload was larger than bufcap. */
int wire_recv_frame_blocking(int fd, uint8_t *type, uint8_t *flags, uint64_t *stream_id,
                              uint8_t *buf, uint32_t bufcap, uint32_t *paylen);

#endif
