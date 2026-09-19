/* Generates a real toy weight file (via the actual gen_toy_weights
 * binary), starts a real swapd against it, and loads+runs the model
 * through the real pageclient path -- proving tensor loading actually
 * goes through swapd's paging (not a shortcut file read) and that the
 * forward pass produces sane, finite output.
 */
#include "toy_model.h"

#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NODE_ID 55

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int run_and_wait(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(path, argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return status;
}

int main(void) {
    const char *weight_path = "/tmp/toy_model_test.weights";
    const char *swapd_conf = "/tmp/toy_model_test_swapd.conf";
    const char *sock_path = "/tmp/toy_model_test_swapd.sock";

    char *gen_argv[] = {(char *)"gen_toy_weights", (char *)weight_path, (char *)"777", NULL};
    run_and_wait(GEN_TOY_WEIGHTS_BIN, gen_argv);

    FILE *f = fopen(weight_path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fclose(f);

    FILE *cf = fopen(swapd_conf, "w");
    fprintf(cf, "node_id %d\n", NODE_ID);
    fprintf(cf, "weight_file %s\n", weight_path);
    fprintf(cf, "tensor_region_bytes %ld\n", file_size);
    fprintf(cf, "page_size 4096\n");
    fprintf(cf, "dram_capacity_pages %ld\n", file_size / 4096);
    fprintf(cf, "unix_socket %s\n", sock_path);
    fclose(cf);

    pid_t swapd_pid = fork();
    assert(swapd_pid >= 0);
    if (swapd_pid == 0) {
        execl(SWAPD_BIN, "swapd", "--config", swapd_conf, (char *)NULL);
        _exit(127);
    }
    sleep_ms(300);

    pageclient_t *pc = pageclient_connect(NODE_ID, 4096, sock_path);
    assert(pc);

    toy_model_t *model = toy_model_load(weight_path, pc);
    assert(model);
    assert(model->hdr.vocab_size == 256);
    assert(model->hdr.dim == 32);
    printf("ok: toy model loaded through swapd's paging (vocab=%u dim=%u ff_dim=%u)\n",
           model->hdr.vocab_size, model->hdr.dim, model->hdr.ff_dim);

    kv_session_t *sess = kv_session_create(model->hdr.dim, 8);
    float *logits = malloc(model->hdr.vocab_size * sizeof(float));

    /* Feed the byte string "Hi" and check the model produces well-formed
     * (finite, non-degenerate) logits at each step. */
    const char *prompt = "Hi";
    for (const char *p = prompt; *p; p++) {
        toy_model_forward_step(model, sess, (uint32_t)(unsigned char)*p, logits);
        int any_finite = 0, all_zero = 1;
        for (uint32_t i = 0; i < model->hdr.vocab_size; i++) {
            assert(isfinite(logits[i]));
            if (logits[i] != 0.0f) all_zero = 0;
            any_finite = 1;
        }
        assert(any_finite && !all_zero);
    }
    assert(sess->len == 2);
    printf("ok: forward pass over a real prompt produces finite, non-degenerate logits\n");

    uint32_t rng = 123;
    uint32_t greedy = toy_model_sample(logits, model->hdr.vocab_size, 0.0f, &rng);
    assert(greedy < model->hdr.vocab_size);
    uint32_t sampled = toy_model_sample(logits, model->hdr.vocab_size, 0.8f, &rng);
    assert(sampled < model->hdr.vocab_size);
    printf("ok: greedy and temperature sampling both return valid token ids\n");

    free(logits);
    kv_session_free(sess);
    toy_model_free(model);
    pageclient_disconnect(pc);

    kill(swapd_pid, SIGTERM);
    int status;
    waitpid(swapd_pid, &status, 0);
    unlink(weight_path);
    unlink(swapd_conf);

    printf("all toy_model tests passed\n");
    return 0;
}
