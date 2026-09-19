/* Round-trip tests for lib/wire: the blocking helpers and the non-blocking
 * buffered wire_conn_t, run over a socketpair() so no network is needed. */
#include "wire.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_blocking_roundtrip(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    const char *msg = "hello continuum";
    assert(wire_send_frame_blocking(fds[0], WIRE_HEARTBEAT, WIRE_FLAG_FINAL, 42, msg, (uint32_t)strlen(msg)) == 0);

    uint8_t type, flags;
    uint64_t stream_id;
    uint8_t buf[256];
    uint32_t paylen;
    assert(wire_recv_frame_blocking(fds[1], &type, &flags, &stream_id, buf, sizeof(buf), &paylen) == 0);
    assert(type == WIRE_HEARTBEAT);
    assert(flags == WIRE_FLAG_FINAL);
    assert(stream_id == 42);
    assert(paylen == strlen(msg));
    assert(memcmp(buf, msg, paylen) == 0);

    close(fds[0]);
    close(fds[1]);
    printf("ok: blocking roundtrip\n");
}

typedef struct {
    int frames_seen;
    uint64_t last_stream_id;
    char last_payload[256];
    uint32_t last_paylen;
} capture_t;

static void capture_cb(void *user, uint8_t type, uint8_t flags, uint64_t stream_id,
                        const uint8_t *payload, uint32_t paylen) {
    (void)type;
    (void)flags;
    capture_t *cap = user;
    cap->frames_seen++;
    cap->last_stream_id = stream_id;
    cap->last_paylen = paylen;
    memcpy(cap->last_payload, payload, paylen);
}

static void test_nonblocking_conn_multi_frame(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    /* fds[1] is read non-blocking through wire_conn_t */
    int flags = fcntl(fds[1], F_GETFL, 0);
    fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);

    wire_conn_t *writer = wire_conn_new(fds[0]);
    assert(wire_conn_enqueue(writer, WIRE_TOKEN_CHUNK, 0, 1, "one", 3) == 0);
    assert(wire_conn_enqueue(writer, WIRE_TOKEN_CHUNK, 0, 2, "two", 3) == 0);
    assert(wire_conn_enqueue(writer, WIRE_TOKEN_CHUNK, WIRE_FLAG_FINAL, 3, "three!", 6) == 0);
    assert(wire_conn_flush(writer) == 0); /* socketpair buffers are plenty for this */

    wire_conn_t *reader = wire_conn_new(fds[1]);
    capture_t cap = {0};
    assert(wire_conn_on_readable(reader, capture_cb, &cap) == 0);
    assert(cap.frames_seen == 3);
    assert(cap.last_stream_id == 3);
    assert(cap.last_paylen == 6);
    assert(memcmp(cap.last_payload, "three!", 6) == 0);

    wire_conn_free(writer);
    wire_conn_free(reader);
    close(fds[0]);
    close(fds[1]);
    printf("ok: non-blocking multi-frame conn\n");
}

static void test_partial_frame_across_two_reads(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    int flags = fcntl(fds[1], F_GETFL, 0);
    fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);

    /* Write only the header + half the payload first. */
    const char *payload = "partialpartialpartial";
    uint32_t paylen = (uint32_t)strlen(payload);
    wire_header_t hdr = {.length = paylen, .type = WIRE_ACTIVATION_HOP, .flags = 0, .reserved = 0, .stream_id = 99};
    write(fds[0], &hdr, WIRE_HEADER_SIZE);
    write(fds[0], payload, 5);

    wire_conn_t *reader = wire_conn_new(fds[1]);
    capture_t cap = {0};
    assert(wire_conn_on_readable(reader, capture_cb, &cap) == 0);
    assert(cap.frames_seen == 0); /* frame incomplete, must not fire early */

    write(fds[0], payload + 5, paylen - 5);
    assert(wire_conn_on_readable(reader, capture_cb, &cap) == 0);
    assert(cap.frames_seen == 1);
    assert(cap.last_paylen == paylen);
    assert(memcmp(cap.last_payload, payload, paylen) == 0);

    wire_conn_free(reader);
    close(fds[0]);
    close(fds[1]);
    printf("ok: partial frame across two reads\n");
}

int main(void) {
    test_blocking_roundtrip();
    test_nonblocking_conn_multi_frame();
    test_partial_frame_across_two_reads();
    printf("all wire tests passed\n");
    return 0;
}
