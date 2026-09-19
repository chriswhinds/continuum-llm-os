#include "wire.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "clog.h"

/* ---- tiny growable byte buffer, used for both recv and send queues ------ */
typedef struct {
    uint8_t *data;
    size_t len;   /* bytes currently holding data */
    size_t cap;
    size_t off;   /* consumed prefix (recv side only) -- compacted lazily */
} buf_t;

static int buf_reserve(buf_t *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t new_cap = b->cap ? b->cap * 2 : 4096;
    while (new_cap < b->len + extra) new_cap *= 2;
    uint8_t *n = realloc(b->data, new_cap);
    if (!n) return -1;
    b->data = n;
    b->cap = new_cap;
    return 0;
}

static void buf_compact(buf_t *b) {
    if (b->off == 0) return;
    memmove(b->data, b->data + b->off, b->len - b->off);
    b->len -= b->off;
    b->off = 0;
}

static int buf_append(buf_t *b, const void *p, size_t n) {
    if (buf_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

static void buf_free(buf_t *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

/* ---- connection ----------------------------------------------------------- */

struct wire_conn {
    int fd;
    buf_t recv;
    buf_t send;
};

wire_conn_t *wire_conn_new(int fd) {
    wire_conn_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = fd;
    return c;
}

void wire_conn_free(wire_conn_t *c) {
    if (!c) return;
    buf_free(&c->recv);
    buf_free(&c->send);
    free(c);
}

int wire_conn_fd(const wire_conn_t *c) { return c->fd; }

int wire_conn_enqueue(wire_conn_t *c, uint8_t type, uint8_t flags,
                       uint64_t stream_id, const void *payload, uint32_t paylen) {
    if (paylen > WIRE_MAX_PAYLOAD) return -1;
    wire_header_t hdr = {
        .length = paylen,
        .type = type,
        .flags = flags,
        .reserved = 0,
        .stream_id = stream_id,
    };
    if (buf_append(&c->send, &hdr, WIRE_HEADER_SIZE) != 0) return -1;
    if (paylen && buf_append(&c->send, payload, paylen) != 0) return -1;
    return 0;
}

int wire_conn_flush(wire_conn_t *c) {
    while (c->send.off < c->send.len) {
        ssize_t n = write(c->fd, c->send.data + c->send.off, c->send.len - c->send.off);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                buf_compact(&c->send);
                return 1; /* still pending */
            }
            if (errno == EINTR) continue;
            return -1;
        }
        c->send.off += (size_t)n;
    }
    c->send.len = 0;
    c->send.off = 0;
    return 0;
}

int wire_conn_has_pending_writes(const wire_conn_t *c) {
    return c->send.off < c->send.len;
}

int wire_conn_on_readable(wire_conn_t *c, wire_frame_cb cb, void *user) {
    buf_compact(&c->recv);

    for (;;) {
        if (buf_reserve(&c->recv, 65536) != 0) {
            errno = ENOMEM;
            return -1;
        }
        size_t avail = c->recv.cap - c->recv.len;
        ssize_t n = read(c->fd, c->recv.data + c->recv.len, avail);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1; /* peer closed */
        }
        c->recv.len += (size_t)n;
        if ((size_t)n < avail) break; /* short read: the socket is drained for now */
    }

    /* Drain as many complete frames as we have. */
    size_t pos = 0;
    for (;;) {
        if (c->recv.len - pos < WIRE_HEADER_SIZE) break;
        wire_header_t hdr;
        memcpy(&hdr, c->recv.data + pos, WIRE_HEADER_SIZE);
        if (hdr.length > WIRE_MAX_PAYLOAD) {
            clog_error("wire: peer sent oversize frame length=%u, dropping connection", hdr.length);
            errno = EPROTO;
            return -1;
        }
        size_t frame_total = WIRE_HEADER_SIZE + hdr.length;
        if (c->recv.len - pos < frame_total) break; /* incomplete, wait for more */

        cb(user, hdr.type, hdr.flags, hdr.stream_id, c->recv.data + pos + WIRE_HEADER_SIZE, hdr.length);
        pos += frame_total;
    }
    c->recv.off = pos;
    buf_compact(&c->recv);
    return 0;
}

/* ---- blocking helpers ----------------------------------------------------- */

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *p, size_t n) {
    uint8_t *b = p;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, b + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            errno = ECONNRESET;
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

int wire_send_frame_blocking(int fd, uint8_t type, uint8_t flags,
                              uint64_t stream_id, const void *payload, uint32_t paylen) {
    if (paylen > WIRE_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    wire_header_t hdr = {.length = paylen, .type = type, .flags = flags, .reserved = 0, .stream_id = stream_id};
    if (write_all(fd, &hdr, WIRE_HEADER_SIZE) != 0) return -1;
    if (paylen && write_all(fd, payload, paylen) != 0) return -1;
    return 0;
}

int wire_recv_frame_blocking(int fd, uint8_t *type, uint8_t *flags, uint64_t *stream_id,
                              uint8_t *buf, uint32_t bufcap, uint32_t *paylen) {
    wire_header_t hdr;
    if (read_all(fd, &hdr, WIRE_HEADER_SIZE) != 0) return -1;
    if (hdr.length > WIRE_MAX_PAYLOAD) {
        errno = EPROTO;
        return -1;
    }
    if (hdr.length > bufcap) {
        /* Drain and discard so the connection stays in a valid framed state. */
        uint8_t scratch[4096];
        uint32_t remaining = hdr.length;
        while (remaining) {
            uint32_t chunk = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
            if (read_all(fd, scratch, chunk) != 0) return -1;
            remaining -= chunk;
        }
        return -2;
    }
    if (hdr.length && read_all(fd, buf, hdr.length) != 0) return -1;
    if (type) *type = hdr.type;
    if (flags) *flags = hdr.flags;
    if (stream_id) *stream_id = hdr.stream_id;
    if (paylen) *paylen = hdr.length;
    return 0;
}
