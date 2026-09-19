/* Full chain against real binaries: gen_toy_weights -> swapd -> shard-execd
 * -> schedulerd, then a client sends a WIRE_SCHED_REQUEST naming the model
 * and gets back the same relayed token stream shard_execd_e2e_test
 * verified talking to shard-execd directly -- this time proving
 * schedulerd's routing-by-model-name and relay logic on top of that.
 */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sched_protocol.h"
#include "sock_util.h"
#include "wire.h"

#define NODE_ID 88

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
    const char *weight_path = "/tmp/sched_e2e.weights";
    const char *swapd_conf = "/tmp/sched_e2e_swapd.conf";
    const char *shard_conf = "/tmp/sched_e2e_shard.conf";
    const char *sched_conf = "/tmp/sched_e2e_sched.conf";
    const char *swapd_sock = "/tmp/sched_e2e_swapd.sock";
    const char *shard_sock = "/tmp/sched_e2e_shard.sock";
    const char *sched_sock = "/tmp/sched_e2e_sched.sock";

    char *gen_argv[] = {(char *)"gen_toy_weights", (char *)weight_path, (char *)"55", NULL};
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
                "listen_unix_socket %s\ndefault_max_new_tokens 8\ndefault_temperature 0\n",
            NODE_ID, weight_path, swapd_sock, shard_sock);
    fclose(cf);

    cf = fopen(sched_conf, "w");
    fprintf(cf, "listen_unix_socket %s\nroute toy-model %s\n", sched_sock, shard_sock);
    fclose(cf);

    char *swapd_argv[] = {(char *)"swapd", (char *)"--config", (char *)swapd_conf, NULL};
    pid_t swapd_pid = spawn(SWAPD_BIN, swapd_argv);
    sleep_ms(300);

    char *shard_argv[] = {(char *)"shard-execd", (char *)"--config", (char *)shard_conf, NULL};
    pid_t shard_pid = spawn(SHARD_EXECD_BIN, shard_argv);
    sleep_ms(500);

    char *sched_argv[] = {(char *)"schedulerd", (char *)"--config", (char *)sched_conf, NULL};
    pid_t sched_pid = spawn(SCHEDULERD_BIN, sched_argv);

    int fd = -1;
    for (int attempt = 0; attempt < 50 && fd < 0; attempt++) {
        sleep_ms(100);
        fd = sock_unix_connect(sched_sock);
    }
    assert(fd >= 0);
    printf("ok: schedulerd came up and accepted a connection\n");

    const char *prompt = "Hey";
    uint32_t n = (uint32_t)strlen(prompt);
    size_t reqlen = sizeof(sched_request_header_t) + n * sizeof(uint32_t);
    uint8_t *reqbuf = malloc(reqlen);
    sched_request_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strncpy(hdr.model_name, "toy-model", sizeof(hdr.model_name) - 1);
    hdr.n_prompt_tokens = n;
    hdr.max_new_tokens = 6;
    hdr.temperature = 0.0f;
    hdr.rng_seed = 1;
    memcpy(reqbuf, &hdr, sizeof(hdr));
    uint32_t *toks = (uint32_t *)(reqbuf + sizeof(hdr));
    for (uint32_t i = 0; i < n; i++) toks[i] = (uint32_t)(unsigned char)prompt[i];

    assert(wire_send_frame_blocking(fd, WIRE_SCHED_REQUEST, 0, 42, reqbuf, (uint32_t)reqlen) == 0);
    free(reqbuf);

    int n_chunks = 0;
    for (;;) {
        uint8_t type, flags;
        uint64_t stream_id;
        uint8_t buf[64];
        uint32_t paylen;
        assert(wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) == 0);
        assert(type == WIRE_TOKEN_CHUNK);
        assert(stream_id == 42); /* schedulerd must preserve the caller's stream_id, not the shard's */
        n_chunks++;
        if (flags & WIRE_FLAG_FINAL) break;
        assert(n_chunks <= 6);
    }
    printf("ok: schedulerd routed by model name and relayed %d token chunk(s) with the correct stream_id\n", n_chunks);

    /* An unknown model must get a clean error, not a hang or crash. */
    close(fd);
    fd = sock_unix_connect(sched_sock);
    assert(fd >= 0);
    sched_request_header_t bad;
    memset(&bad, 0, sizeof(bad));
    strncpy(bad.model_name, "no-such-model", sizeof(bad.model_name) - 1);
    assert(wire_send_frame_blocking(fd, WIRE_SCHED_REQUEST, 0, 1, &bad, sizeof(bad)) == 0);
    uint8_t type, flags;
    uint64_t stream_id;
    uint8_t buf[128];
    uint32_t paylen;
    assert(wire_recv_frame_blocking(fd, &type, &flags, &stream_id, buf, sizeof(buf), &paylen) == 0);
    assert(type == WIRE_ERROR);
    printf("ok: unknown model name gets a clean WIRE_ERROR response\n");

    close(fd);
    kill(sched_pid, SIGTERM);
    kill(shard_pid, SIGTERM);
    kill(swapd_pid, SIGTERM);
    waitpid(sched_pid, &status, 0);
    waitpid(shard_pid, &status, 0);
    waitpid(swapd_pid, &status, 0);
    unlink(weight_path);
    unlink(swapd_conf);
    unlink(shard_conf);
    unlink(sched_conf);

    printf("all scheduler e2e tests passed\n");
    return 0;
}
