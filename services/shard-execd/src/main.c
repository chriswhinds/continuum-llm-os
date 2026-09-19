/* shard-execd -- ARCH-002 §05: loads its model's tensors through swapd
 * (via pageclient, see toy_model.c) and runs the forward/decode loop for
 * one request at a time, streaming generated tokens back to whoever
 * dispatched the prompt (schedulerd in the full pipeline; the CLI tool in
 * tools/continuum-cli for direct testing).
 *
 * Scope: single connection handled synchronously, no concurrent session
 * multiplexing or cross-request batching -- ARCH-002's scheduler-side
 * batching (§05) is what would normally keep this busy; this reference
 * build proves the paging + forward-pass path end to end rather than
 * building a production request scheduler inside shard-execd itself.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "clog.h"
#include "dispatch_protocol.h"
#include "kvcache.h"
#include "pageclient.h"
#include "shard_config.h"
#include "sock_util.h"
#include "toy_model.h"
#include "wire.h"

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void handle_connection(int fd, const toy_model_t *model, kvcache_t *kv, const shard_config_t *cfg) {
    uint8_t type, flags;
    uint64_t stream_id;
    uint32_t paylen;
    size_t bufcap = sizeof(dispatch_prompt_header_t) + 65536 * sizeof(uint32_t);
    uint8_t *buf = malloc(bufcap);

    if (wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, (uint32_t)bufcap, &paylen) != 0) {
        free(buf);
        return;
    }
    if (type != WIRE_DISPATCH_PROMPT || paylen < sizeof(dispatch_prompt_header_t)) {
        clog_warn("shard-execd: unexpected frame type %u on dispatch socket", type);
        free(buf);
        return;
    }

    dispatch_prompt_header_t req;
    memcpy(&req, buf, sizeof(req));
    const uint32_t *prompt = (const uint32_t *)(buf + sizeof(req));
    uint32_t max_new = req.max_new_tokens ? req.max_new_tokens : cfg->default_max_new_tokens;
    float temperature = req.temperature != 0.0f ? req.temperature : cfg->default_temperature;
    uint32_t rng_state = req.rng_seed ? req.rng_seed : 1u;

    /* Prefix-cache lookup purely for observability in this reference
     * build -- see toy_model.h's module comment on why matched positions
     * aren't yet used to skip recomputation. */
    uint64_t *dummy_pages = malloc((req.n_prompt_tokens + max_new) * sizeof(uint64_t));
    size_t matched = kvcache_lookup_prefix(kv, prompt, req.n_prompt_tokens, dummy_pages, now_ns());
    if (matched > 0) {
        clog_info("shard-execd: prefix cache matched %zu/%u prompt tokens (recompute not yet skipped)",
                  matched, req.n_prompt_tokens);
    }

    kv_session_t *sess = kv_session_create(model->hdr.dim, req.n_prompt_tokens + max_new);
    float *logits = malloc(model->hdr.vocab_size * sizeof(float));

    uint32_t *full_seq = malloc((req.n_prompt_tokens + max_new) * sizeof(uint32_t));
    memcpy(full_seq, prompt, req.n_prompt_tokens * sizeof(uint32_t));
    uint32_t seq_len = req.n_prompt_tokens;

    /* Prefill: run every prompt token through the model to build up KV
     * history; only the logits from the LAST prompt token matter for
     * choosing the first generated token. */
    uint32_t last_token = 0;
    for (uint32_t i = 0; i < req.n_prompt_tokens; i++) {
        last_token = prompt[i];
        toy_model_forward_step(model, sess, last_token, logits);
    }

    for (uint32_t step = 0; step < max_new; step++) {
        uint32_t next = toy_model_sample(logits, model->hdr.vocab_size, temperature, &rng_state);
        full_seq[seq_len++] = next;

        int is_last = (step + 1 == max_new) || (next == 0 /* token 0 doubles as EOS */);
        token_chunk_t chunk = {.token = next};
        wire_send_frame_blocking(fd, WIRE_TOKEN_CHUNK, is_last ? WIRE_FLAG_FINAL : 0, stream_id, &chunk, sizeof(chunk));

        if (is_last) break;
        toy_model_forward_step(model, sess, next, logits);
    }

    for (uint32_t i = 0; i < seq_len; i++) dummy_pages[i] = i; /* opaque handles, see module comment */
    kvcache_insert(kv, full_seq, dummy_pages, seq_len, now_ns());

    free(dummy_pages);
    free(full_seq);
    free(logits);
    kv_session_free(sess);
    free(buf);
}

int main(int argc, char **argv) {
    clog_init("shard-execd");
    const char *config_path = "/etc/continuum/shard-execd.conf";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) clog_set_level(CLOG_DEBUG);
    }

    shard_config_t cfg;
    if (shard_config_load(config_path, &cfg) != 0) return 1;

    pageclient_t *pc = pageclient_connect(cfg.node_id, cfg.page_size, cfg.swapd_unix_socket);
    if (!pc) {
        clog_error("shard-execd: could not attach to swapd's shared state for node %u", cfg.node_id);
        return 1;
    }

    toy_model_t *model = toy_model_load(cfg.weight_file, pc);
    if (!model) {
        clog_error("shard-execd: failed to load model from %s", cfg.weight_file);
        return 1;
    }

    kvcache_t *kv = kvcache_create(65536);

    /* TCP, not Unix -- schedulerd dials in from a control-plane node,
     * shard-execd runs on a compute node (different physical boards per
     * ARCH-002 §01); see shard_config.h's module comment. */
    int listen_fd = sock_tcp_listen(NULL, cfg.listen_port, 8);
    if (listen_fd < 0) {
        clog_error("shard-execd: cannot listen on :%u: %s", cfg.listen_port, strerror(errno));
        return 1;
    }
    /* shard-execd handles one connection at a time synchronously (see the
     * module comment), so a plain blocking accept()/recv() loop is the
     * right shape here -- undo sock_tcp_listen()'s O_NONBLOCK, which
     * exists for the epoll-driven daemons (node-agentd, swapd), not this
     * one. */
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags & ~O_NONBLOCK);

    clog_info("shard-execd: node %u ready, listening on :%u", cfg.node_id, cfg.listen_port);

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            clog_error("shard-execd: accept() failed: %s", strerror(errno));
            break;
        }
        handle_connection(fd, model, kv, &cfg);
        close(fd);
    }

    kvcache_destroy(kv);
    toy_model_free(model);
    pageclient_disconnect(pc);
    return 0;
}
