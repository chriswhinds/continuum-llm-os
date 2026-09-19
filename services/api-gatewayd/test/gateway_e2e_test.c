/* The capstone test: the entire chain from a real HTTP request down to
 * paged tensor bytes and back. gen_toy_weights -> swapd -> shard-execd ->
 * schedulerd -> api-gatewayd, then a raw HTTP/1.1 client (no curl
 * dependency -- just sockets) exercises GET /v1/models and POST
 * /v1/chat/completions exactly the way an OpenAI SDK would, and checks
 * the JSON response shape.
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

#define NODE_ID 99
#define HTTP_PORT 18099

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

/* Sends a raw HTTP request and reads the full response (until the peer
 * closes -- every response in this test sets Connection: close). */
static void http_roundtrip(const char *request, char *response, size_t response_cap) {
    int fd = -1;
    for (int attempt = 0; attempt < 50 && fd < 0; attempt++) {
        fd = sock_tcp_connect("127.0.0.1", HTTP_PORT);
        if (fd < 0) sleep_ms(100);
    }
    assert(fd >= 0);
    write(fd, request, strlen(request));

    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, response + total, response_cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= response_cap - 1) break;
    }
    response[total] = '\0';
    close(fd);
}

int main(void) {
    const char *weight_path = "/tmp/gw_e2e.weights";
    const char *swapd_conf = "/tmp/gw_e2e_swapd.conf";
    const char *shard_conf = "/tmp/gw_e2e_shard.conf";
    const char *sched_conf = "/tmp/gw_e2e_sched.conf";
    const char *gw_conf = "/tmp/gw_e2e_gateway.conf";
    const char *swapd_sock = "/tmp/gw_e2e_swapd.sock";
    const char *shard_sock = "/tmp/gw_e2e_shard.sock";
    const char *sched_sock = "/tmp/gw_e2e_sched.sock";

    char *gen_argv[] = {(char *)"gen_toy_weights", (char *)weight_path, (char *)"7", NULL};
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

    cf = fopen(gw_conf, "w");
    fprintf(cf, "listen_port %d\nscheduler_unix_socket %s\nmodel toy-model\n", HTTP_PORT, sched_sock);
    fclose(cf);

    char *swapd_argv[] = {(char *)"swapd", (char *)"--config", (char *)swapd_conf, NULL};
    pid_t swapd_pid = spawn(SWAPD_BIN, swapd_argv);
    sleep_ms(300);
    char *shard_argv[] = {(char *)"shard-execd", (char *)"--config", (char *)shard_conf, NULL};
    pid_t shard_pid = spawn(SHARD_EXECD_BIN, shard_argv);
    sleep_ms(500);
    char *sched_argv[] = {(char *)"schedulerd", (char *)"--config", (char *)sched_conf, NULL};
    pid_t sched_pid = spawn(SCHEDULERD_BIN, sched_argv);
    sleep_ms(200);
    char *gw_argv[] = {(char *)"api-gatewayd", (char *)"--config", (char *)gw_conf, NULL};
    pid_t gw_pid = spawn(API_GATEWAYD_BIN, gw_argv);

    char response[16384];

    http_roundtrip("GET /v1/models HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
                   response, sizeof(response));
    assert(strstr(response, "200") != NULL);
    assert(strstr(response, "\"object\":\"list\"") != NULL);
    assert(strstr(response, "\"id\":\"toy-model\"") != NULL);
    printf("ok: GET /v1/models returns the configured model\n");

    const char *chat_body = "{\"model\":\"toy-model\",\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],"
                             "\"max_tokens\":5,\"temperature\":0}";
    char chat_req[1024];
    snprintf(chat_req, sizeof(chat_req),
             "POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
             strlen(chat_body), chat_body);
    http_roundtrip(chat_req, response, sizeof(response));
    assert(strstr(response, "200") != NULL);
    assert(strstr(response, "\"object\":\"chat.completion\"") != NULL);
    assert(strstr(response, "\"role\":\"assistant\"") != NULL);
    assert(strstr(response, "\"finish_reason\":\"stop\"") != NULL);
    printf("ok: POST /v1/chat/completions (non-streaming) returns a real OpenAI-shaped completion\n");
    printf("    response body: %s\n", strstr(response, "\r\n\r\n") + 4);

    const char *stream_body = "{\"model\":\"toy-model\",\"messages\":[{\"role\":\"user\",\"content\":\"Yo\"}],"
                               "\"max_tokens\":4,\"temperature\":0,\"stream\":true}";
    char stream_req[1024];
    snprintf(stream_req, sizeof(stream_req),
             "POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
             strlen(stream_body), stream_body);
    http_roundtrip(stream_req, response, sizeof(response));
    assert(strstr(response, "text/event-stream") != NULL);
    assert(strstr(response, "chat.completion.chunk") != NULL);
    assert(strstr(response, "data: [DONE]") != NULL);
    printf("ok: POST /v1/chat/completions (stream=true) returns a real SSE token stream\n");

    /* Unknown model must surface as a clean HTTP error, not a hang. */
    const char *bad_body = "{\"model\":\"no-such-model\",\"messages\":[{\"role\":\"user\",\"content\":\"x\"}]}";
    char bad_req[512];
    snprintf(bad_req, sizeof(bad_req),
             "POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
             strlen(bad_body), bad_body);
    http_roundtrip(bad_req, response, sizeof(response));
    assert(strstr(response, "502") != NULL);
    printf("ok: an unroutable model name surfaces as a clean HTTP error\n");

    kill(gw_pid, SIGTERM);
    kill(sched_pid, SIGTERM);
    kill(shard_pid, SIGTERM);
    kill(swapd_pid, SIGTERM);
    waitpid(gw_pid, &status, 0);
    waitpid(sched_pid, &status, 0);
    waitpid(shard_pid, &status, 0);
    waitpid(swapd_pid, &status, 0);
    unlink(weight_path);
    unlink(swapd_conf);
    unlink(shard_conf);
    unlink(sched_conf);
    unlink(gw_conf);

    printf("all gateway e2e tests passed -- full HTTP-to-paged-tensor pipeline verified\n");
    return 0;
}
