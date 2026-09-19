/* Full pipeline end-to-end test against REAL binaries: gen_toy_weights ->
 * swapd -> shard-execd, then a client dispatches a prompt over TCP (the
 * same link schedulerd uses -- see shard_config.h on why this is TCP,
 * not Unix) and streams tokens back. This is the
 * strongest test in the tree: it proves paging, model loading, the
 * forward/decode loop, and the wire protocol all work together, not just
 * each piece in isolation.
 */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dispatch_protocol.h"
#include "sock_util.h"
#include "wire.h"

#define NODE_ID 66
#define SHARD_PORT 18801

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static pid_t spawn(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(path, argv);
        _exit(127);
    }
    return pid;
}

int main(void) {
    const char *weight_path = "/tmp/shard_e2e_test.weights";
    const char *swapd_conf = "/tmp/shard_e2e_test_swapd.conf";
    const char *shard_conf = "/tmp/shard_e2e_test_shard.conf";
    const char *swapd_sock = "/tmp/shard_e2e_test_swapd.sock";

    char *gen_argv[] = {(char *)"gen_toy_weights", (char *)weight_path, (char *)"99", NULL};
    pid_t gen_pid = spawn(GEN_TOY_WEIGHTS_BIN, gen_argv);
    int status;
    waitpid(gen_pid, &status, 0);

    FILE *f = fopen(weight_path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fclose(f);

    FILE *cf = fopen(swapd_conf, "w");
    fprintf(cf, "node_id %d\nweight_file %s\ntensor_region_bytes %ld\npage_size 4096\n"
                "dram_capacity_pages %ld\nunix_socket %s\n",
            NODE_ID, weight_path, file_size, file_size / 4096, swapd_sock);
    fclose(cf);

    cf = fopen(shard_conf, "w");
    fprintf(cf, "node_id %d\nweight_file %s\npage_size 4096\nswapd_unix_socket %s\n"
                "listen_port %d\ndefault_max_new_tokens 12\ndefault_temperature 0\n",
            NODE_ID, weight_path, swapd_sock, SHARD_PORT);
    fclose(cf);

    char *swapd_argv[] = {(char *)"swapd", (char *)"--config", (char *)swapd_conf, NULL};
    pid_t swapd_pid = spawn(SWAPD_BIN, swapd_argv);
    sleep_ms(300);

    char *shard_argv[] = {(char *)"shard-execd", (char *)"--config", (char *)shard_conf, NULL};
    pid_t shard_pid = spawn(SHARD_EXECD_BIN, shard_argv);

    int fd = -1;
    for (int attempt = 0; attempt < 50 && fd < 0; attempt++) {
        sleep_ms(100);
        fd = sock_tcp_connect("127.0.0.1", SHARD_PORT);
    }
    assert(fd >= 0);
    printf("ok: shard-execd came up (model loaded through swapd) and accepted a connection\n");

    /* Dispatch the byte-string prompt "Hello" with greedy sampling
     * (temperature 0) so the run is deterministic. */
    const char *prompt = "Hello";
    uint32_t n = (uint32_t)strlen(prompt);
    size_t reqlen = sizeof(dispatch_prompt_header_t) + n * sizeof(uint32_t);
    uint8_t *reqbuf = malloc(reqlen);
    dispatch_prompt_header_t hdr = {.n_prompt_tokens = n, .max_new_tokens = 10, .temperature = 0.0f, .rng_seed = 1};
    memcpy(reqbuf, &hdr, sizeof(hdr));
    uint32_t *toks = (uint32_t *)(reqbuf + sizeof(hdr));
    for (uint32_t i = 0; i < n; i++) toks[i] = (uint32_t)(unsigned char)prompt[i];

    assert(wire_send_frame_blocking(fd, WIRE_DISPATCH_PROMPT, 0, 7, reqbuf, (uint32_t)reqlen) == 0);
    free(reqbuf);

    int n_chunks = 0;
    for (;;) {
        uint8_t type, flags;
        uint64_t stream_id;
        uint8_t buf[64];
        uint32_t paylen;
        assert(wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) == 0);
        assert(type == WIRE_TOKEN_CHUNK);
        assert(stream_id == 7);
        token_chunk_t chunk;
        memcpy(&chunk, buf, sizeof(chunk));
        assert(chunk.token < 256);
        n_chunks++;
        if (flags & WIRE_FLAG_FINAL) break;
        assert(n_chunks <= 10); /* max_new_tokens -- guards against an infinite loop on a bug */
    }
    printf("ok: streamed %d token chunk(s) ending with WIRE_FLAG_FINAL, as dispatched\n", n_chunks);

    close(fd);
    kill(shard_pid, SIGTERM);
    kill(swapd_pid, SIGTERM);
    waitpid(shard_pid, &status, 0);
    waitpid(swapd_pid, &status, 0);
    unlink(weight_path);
    unlink(swapd_conf);
    unlink(shard_conf);

    printf("all shard-execd e2e tests passed\n");
    return 0;
}
