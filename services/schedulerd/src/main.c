/* schedulerd -- ARCH-002 §05: routes an incoming request to the
 * shard-execd serving its model, and relays the streamed token response
 * back. See scheduler_config.h for why this reference build's placement
 * is the single-candidate degenerate case of ARCH-001's batching
 * scheduler.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "clog.h"
#include "dispatch_protocol.h"
#include "scheduler_config.h"
#include "sched_protocol.h"
#include "sock_util.h"
#include "wire.h"

static void handle_connection(int client_fd, const scheduler_config_t *cfg) {
    uint8_t type, flags;
    uint64_t stream_id;
    uint32_t paylen;
    size_t bufcap = sizeof(sched_request_header_t) + 65536 * sizeof(uint32_t);
    uint8_t *buf = malloc(bufcap);

    if (wire_recv_frame_blocking(client_fd, &type, &flags, &stream_id, buf, (uint32_t)bufcap, &paylen) != 0) {
        free(buf);
        return;
    }
    if (type != WIRE_SCHED_REQUEST || paylen < sizeof(sched_request_header_t)) {
        clog_warn("schedulerd: unexpected frame type %u", type);
        free(buf);
        return;
    }

    sched_request_header_t req;
    memcpy(&req, buf, sizeof(req));
    req.model_name[SCHED_MODEL_NAME_MAX - 1] = '\0';

    const scheduler_route_t *route = scheduler_find_route(cfg, req.model_name);
    if (!route) {
        clog_warn("schedulerd: no route for model '%s'", req.model_name);
        const char *msg = "model not found";
        wire_send_frame_blocking(client_fd, WIRE_ERROR, WIRE_FLAG_FINAL, stream_id, msg, (uint32_t)strlen(msg));
        free(buf);
        return;
    }

    int shard_fd = sock_unix_connect(route->shard_execd_socket);
    if (shard_fd < 0) {
        clog_error("schedulerd: cannot reach shard-execd at %s for model '%s'", route->shard_execd_socket, req.model_name);
        const char *msg = "shard executor unreachable";
        wire_send_frame_blocking(client_fd, WIRE_ERROR, WIRE_FLAG_FINAL, stream_id, msg, (uint32_t)strlen(msg));
        free(buf);
        return;
    }

    /* sched_request_header_t and dispatch_prompt_header_t agree on every
     * field after the model name (see sched_protocol.h) -- the token
     * array that follows is identical either way, so this is a
     * pass-through re-encode of the header plus a byte-range copy of the
     * tokens, not a re-parse. */
    dispatch_prompt_header_t dreq = {
        .n_prompt_tokens = req.n_prompt_tokens,
        .max_new_tokens = req.max_new_tokens,
        .temperature = req.temperature,
        .rng_seed = req.rng_seed,
    };
    size_t tokens_bytes = (size_t)req.n_prompt_tokens * sizeof(uint32_t);
    size_t dreqlen = sizeof(dreq) + tokens_bytes;
    uint8_t *dreqbuf = malloc(dreqlen);
    memcpy(dreqbuf, &dreq, sizeof(dreq));
    memcpy(dreqbuf + sizeof(dreq), buf + sizeof(req), tokens_bytes);

    int rc = wire_send_frame_blocking(shard_fd, WIRE_DISPATCH_PROMPT, 0, stream_id, dreqbuf, (uint32_t)dreqlen);
    free(dreqbuf);
    free(buf);
    if (rc != 0) {
        close(shard_fd);
        return;
    }

    clog_info("schedulerd: dispatched %u-token prompt for model '%s' to %s",
              req.n_prompt_tokens, req.model_name, route->shard_execd_socket);

    /* Relay every TOKEN_CHUNK straight through until the shard marks one
     * final, streaming as we go rather than buffering the whole response. */
    for (;;) {
        uint8_t rtype, rflags;
        uint64_t rstream;
        uint8_t chunkbuf[64];
        uint32_t rpaylen;
        if (wire_recv_frame_blocking(shard_fd, &rtype, &rflags, &rstream, chunkbuf, sizeof(chunkbuf), &rpaylen) != 0) {
            break;
        }
        wire_send_frame_blocking(client_fd, rtype, rflags, stream_id, chunkbuf, rpaylen);
        if (rflags & WIRE_FLAG_FINAL) break;
    }
    close(shard_fd);
}

int main(int argc, char **argv) {
    clog_init("schedulerd");
    const char *config_path = "/etc/continuum/schedulerd.conf";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) clog_set_level(CLOG_DEBUG);
    }

    scheduler_config_t cfg;
    if (scheduler_config_load(config_path, &cfg) != 0) return 1;

    int listen_fd = sock_unix_listen(cfg.listen_unix_socket, 16);
    if (listen_fd < 0) {
        clog_error("schedulerd: cannot listen on %s: %s", cfg.listen_unix_socket, strerror(errno));
        return 1;
    }
    /* Synchronous, one-connection-at-a-time design (see module comment) --
     * undo sock_unix_listen()'s O_NONBLOCK, meant for the epoll-driven
     * daemons. */
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags & ~O_NONBLOCK);

    clog_info("schedulerd: ready with %zu route(s), listening on %s", cfg.n_routes, cfg.listen_unix_socket);

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            clog_error("schedulerd: accept() failed: %s", strerror(errno));
            break;
        }
        handle_connection(fd, &cfg);
        close(fd);
    }
    return 0;
}
