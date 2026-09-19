/* Runs a REAL swapd subprocess against a real weight file, then confirms
 * pageclient_get_page() -- what shard-execd actually calls -- returns
 * correct bytes both on a cold miss (swapd has to page it in) and a
 * repeat hit (already published, straight from shared memory).
 */
#include "pageclient.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NODE_ID 77
#define PAGE_SIZE 4096
#define N_PAGES 4

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int main(void) {
    const char *weight_path = "/tmp/pageclient_test.weights";
    const char *swapd_conf = "/tmp/pageclient_test_swapd.conf";
    const char *sock_path = "/tmp/pageclient_test_swapd.sock";

    FILE *f = fopen(weight_path, "wb");
    uint8_t *buf = malloc(PAGE_SIZE);
    for (int i = 0; i < N_PAGES; i++) {
        memset(buf, 0x10 + i, PAGE_SIZE);
        fwrite(buf, 1, PAGE_SIZE, f);
    }
    fclose(f);

    f = fopen(swapd_conf, "w");
    fprintf(f, "node_id %d\n", NODE_ID);
    fprintf(f, "weight_file %s\n", weight_path);
    fprintf(f, "tensor_region_bytes %d\n", N_PAGES * PAGE_SIZE);
    fprintf(f, "page_size %d\n", PAGE_SIZE);
    fprintf(f, "dram_capacity_pages %d\n", N_PAGES);
    fprintf(f, "unix_socket %s\n", sock_path);
    fclose(f);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl(SWAPD_BIN, "swapd", "--config", swapd_conf, (char *)NULL);
        _exit(127);
    }
    sleep_ms(300); /* let swapd create its shared state and socket */

    pageclient_t *pc = NULL;
    for (int attempt = 0; attempt < 20 && !pc; attempt++) {
        pc = pageclient_connect(NODE_ID, PAGE_SIZE, sock_path);
        if (!pc) sleep_ms(100);
    }
    assert(pc);
    printf("ok: pageclient attached to swapd's shared state\n");

    /* Cold miss on page 2 -- pageclient must reach out to swapd itself. */
    const void *p2 = pageclient_get_page(pc, 2);
    assert(p2 != NULL);
    const uint8_t *bytes2 = p2;
    for (int i = 0; i < PAGE_SIZE; i++) assert(bytes2[i] == 0x12);
    printf("ok: cold miss resolved through swapd with correct content\n");

    /* Repeat request: must be an instant shared-memory hit, same content. */
    const void *p2_again = pageclient_get_page(pc, 2);
    assert(p2_again == p2); /* same slot, zero-copy */
    printf("ok: repeat request is a zero-copy shared-memory hit\n");

    const void *p0 = pageclient_get_page(pc, 0);
    assert(p0 != NULL);
    const uint8_t *bytes0 = p0;
    for (int i = 0; i < PAGE_SIZE; i++) assert(bytes0[i] == 0x10);
    printf("ok: a second distinct page resolves correctly too\n");

    pageclient_disconnect(pc);
    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);
    unlink(weight_path);
    unlink(swapd_conf);

    printf("all pageclient tests passed\n");
    return 0;
}
