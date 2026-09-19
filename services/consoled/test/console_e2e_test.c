/* Real pipeline: gen_toy_weights -> swapd -> node-agentd (health reports
 * every 300ms) -> consoled, then a raw HTTP client polls GET /api/nodes
 * until the node's real telemetry shows up, and checks GET / serves the
 * console page. Proves the WIRE_HEALTH_REPORT path node-agentd already
 * sends (node_agentd_e2e_test covers its server side) actually reaches
 * and is rendered by the operator console.
 */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sock_util.h"

#define NODE_ID 33
#define TELEMETRY_PORT 18401
#define HTTP_PORT 18500

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

static void http_get(int port, const char *path, char *response, size_t cap) {
    int fd = -1;
    for (int attempt = 0; attempt < 50 && fd < 0; attempt++) {
        fd = sock_tcp_connect("127.0.0.1", port);
        if (fd < 0) sleep_ms(100);
    }
    assert(fd >= 0);
    char req[256];
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
    write(fd, req, strlen(req));
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, response + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= cap - 1) break;
    }
    response[total] = '\0';
    close(fd);
}

int main(void) {
    const char *weight_path = "/tmp/console_e2e.weights";
    const char *swapd_conf = "/tmp/console_e2e_swapd.conf";
    const char *na_conf = "/tmp/console_e2e_na.conf";
    const char *console_conf = "/tmp/console_e2e_console.conf";
    const char *swapd_sock = "/tmp/console_e2e_swapd.sock";

    char *gen_argv[] = {(char *)"gen_toy_weights", (char *)weight_path, (char *)"3", NULL};
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

    cf = fopen(na_conf, "w");
    fprintf(cf, "node_id %d\nlisten_port 18301\npage_size 4096\nhealth_interval_ms 300\n"
                "telemetry_sink 127.0.0.1:%d\n",
            NODE_ID, TELEMETRY_PORT);
    fclose(cf);

    cf = fopen(console_conf, "w");
    fprintf(cf, "http_listen_port %d\ntelemetry_listen_port %d\n", HTTP_PORT, TELEMETRY_PORT);
    fclose(cf);

    char *console_argv[] = {(char *)"consoled", (char *)"--config", (char *)console_conf, NULL};
    pid_t console_pid = spawn(CONSOLED_BIN, console_argv);
    sleep_ms(300);

    char *swapd_argv[] = {(char *)"swapd", (char *)"--config", (char *)swapd_conf, NULL};
    pid_t swapd_pid = spawn(SWAPD_BIN, swapd_argv);
    sleep_ms(300);

    char *na_argv[] = {(char *)"node-agentd", (char *)"--config", (char *)na_conf, NULL};
    pid_t na_pid = spawn(NODE_AGENTD_BIN, na_argv);

    char response[8192];
    int seen = 0;
    for (int attempt = 0; attempt < 50 && !seen; attempt++) {
        sleep_ms(200);
        http_get(HTTP_PORT, "/api/nodes", response, sizeof(response));
        char needle[32];
        snprintf(needle, sizeof(needle), "\"node_id\":%d", NODE_ID);
        if (strstr(response, needle)) seen = 1;
    }
    assert(seen);
    assert(strstr(response, "dram_total_bytes") != NULL);
    printf("ok: consoled's /api/nodes shows real telemetry from a real node-agentd within a few health ticks\n");

    http_get(HTTP_PORT, "/", response, sizeof(response));
    assert(strstr(response, "Continuum") != NULL);
    assert(strstr(response, "text/html") != NULL);
    printf("ok: GET / serves the operator console page\n");

    kill(na_pid, SIGTERM);
    kill(swapd_pid, SIGTERM);
    kill(console_pid, SIGTERM);
    waitpid(na_pid, &status, 0);
    waitpid(swapd_pid, &status, 0);
    waitpid(console_pid, &status, 0);
    unlink(weight_path);
    unlink(swapd_conf);
    unlink(na_conf);
    unlink(console_conf);

    printf("all consoled e2e tests passed\n");
    return 0;
}
