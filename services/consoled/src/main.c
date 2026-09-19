/* consoled -- ARCH-002 §05 / ARCH-001 §10: the operator's one screen --
 * cluster-wide node health, aggregated from node-agentd's WIRE_HEALTH_
 * REPORT telemetry (ARCH-002 §05's node-agentd sends these to configured
 * `telemetry_sink` addresses; point them at this process).
 *
 * Two scope notes:
 *
 *   - Browser push: ARCH-002 describes a WebSocket fed by a shared-memory
 *     ring buffer. This reference build's browser side instead polls a
 *     plain JSON HTTP endpoint once a second (console_page.h) -- the
 *     operator-facing result (a live-updating one-screen view) is the
 *     same, and it avoids a separate websocket handshake/frame protocol
 *     for a dashboard that doesn't need sub-second latency.
 *
 *   - HTTP server: originally targeted civetweb (ARCH-002 §02), but the
 *     vendored third_party/civetweb snapshot turned out to have a genuine
 *     syntax error in its request parser -- a broken upstream commit, not
 *     something this build introduced. consoled's actual HTTP surface is
 *     two bodyless GET routes, so rather than chase down a working
 *     civetweb commit, this reuses picohttpparser -- the same real
 *     dependency api-gatewayd already uses for a considerably larger HTTP
 *     surface -- with a small hand-rolled single-threaded server loop in
 *     place of civetweb's.
 *
 * The telemetry listener (plain blocking TCP, one WIRE_HEALTH_REPORT per
 * connection, matching node-agentd's sender) and the HTTP server share
 * this process via two threads.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "clog.h"
#include "console_config.h"
#include "console_page.h"
#include "picohttpparser.h"
#include "sock_util.h"
#include "telemetry_store.h"
#include "wire.h"

static telemetry_store_t *g_store;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void respond(int fd, const char *content_type, const char *body, size_t body_len) {
    char header[128];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      content_type, body_len);
    write(fd, header, (size_t)n);
    if (body_len) write(fd, body, body_len);
}

static void respond_404(int fd) {
    const char *body = "not found";
    char header[96];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 404 Not Found\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
    write(fd, header, (size_t)n);
    write(fd, body, strlen(body));
}

static void handle_api_nodes(int fd) {
    node_stats_entry_t entries[256];
    size_t n = telemetry_store_snapshot(g_store, entries, 256);

    char *body = malloc(n * 256 + 16);
    size_t pos = 0;
    pos += (size_t)sprintf(body + pos, "[");
    for (size_t i = 0; i < n; i++) {
        pos += (size_t)sprintf(body + pos,
            "%s{\"node_id\":%u,\"cpu_load_pct\":%.1f,\"dram_used_bytes\":%llu,"
            "\"dram_total_bytes\":%llu,\"swap_io_bytes_per_sec\":%llu,\"last_seen_ms\":%llu}",
            i > 0 ? "," : "", entries[i].node_id, entries[i].stats.cpu_load_pct,
            (unsigned long long)entries[i].stats.dram_used_bytes,
            (unsigned long long)entries[i].stats.dram_total_bytes,
            (unsigned long long)entries[i].stats.swap_io_bytes_per_sec,
            (unsigned long long)(entries[i].stats.last_seen_ns / 1000000));
    }
    pos += (size_t)sprintf(body + pos, "]");

    respond(fd, "application/json", body, pos);
    free(body);
}

/* One connection, one request -- consoled's whole HTTP surface is two
 * bodyless GET routes, so this skips api-gatewayd's incremental-body-read
 * machinery entirely (there's no body to read). */
static void handle_http_connection(int fd) {
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    const char *method, *path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[16];
    size_t n_headers = 16;
    int parsed = phr_parse_request(buf, (size_t)n, &method, &method_len, &path, &path_len,
                                    &minor_version, headers, &n_headers, 0);
    if (parsed <= 0) return;

    char path_buf[128];
    size_t pl = path_len < sizeof(path_buf) - 1 ? path_len : sizeof(path_buf) - 1;
    memcpy(path_buf, path, pl);
    path_buf[pl] = '\0';

    if (method_len == 3 && strncmp(method, "GET", 3) == 0 && strcmp(path_buf, "/api/nodes") == 0) {
        handle_api_nodes(fd);
    } else if (method_len == 3 && strncmp(method, "GET", 3) == 0 && strcmp(path_buf, "/") == 0) {
        respond(fd, "text/html", CONSOLE_INDEX_HTML, sizeof(CONSOLE_INDEX_HTML) - 1);
    } else {
        respond_404(fd);
    }
}

static void *run_http_server(void *arg) {
    uint16_t port = *(uint16_t *)arg;
    int listen_fd = sock_tcp_listen(NULL, port, 32);
    if (listen_fd < 0) {
        clog_error("consoled: cannot listen for HTTP on :%u: %s", port, strerror(errno));
        return NULL;
    }
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags & ~O_NONBLOCK);

    clog_info("consoled: operator console ready at http://0.0.0.0:%u/", port);

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_http_connection(fd);
        close(fd);
    }
    return NULL;
}

/* Blocking, one-shot-per-connection, matching how node-agentd's
 * send_health_report() dials in: connect, send one frame, close. Runs on
 * the main thread while the HTTP server (above) runs on its own. */
static void run_telemetry_listener(uint16_t port) {
    int listen_fd = sock_tcp_listen(NULL, port, 32);
    if (listen_fd < 0) {
        clog_error("consoled: cannot listen for telemetry on :%u: %s", port, strerror(errno));
        return;
    }
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags & ~O_NONBLOCK);

    clog_info("consoled: telemetry ingestion listening on :%u", port);

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        uint8_t type, wflags;
        uint64_t stream_id;
        uint8_t buf[64];
        uint32_t paylen;
        if (wire_recv_frame_blocking(fd, &type, &wflags, &stream_id, buf, sizeof(buf), &paylen) == 0 &&
            type == WIRE_HEALTH_REPORT && paylen >= sizeof(wire_health_report_t)) {
            wire_health_report_t report;
            memcpy(&report, buf, sizeof(report));
            node_stats_t stats = {
                .cpu_load_pct = report.cpu_load_pct,
                .dram_used_bytes = report.dram_used_bytes,
                .dram_total_bytes = report.dram_total_bytes,
                .swap_io_bytes_per_sec = report.swap_io_bytes_per_sec,
                .last_seen_ns = now_ms() * 1000000ULL,
            };
            telemetry_store_update(g_store, report.node_id, &stats);
        }
        close(fd);
    }
}

int main(int argc, char **argv) {
    clog_init("consoled");
    const char *config_path = "/etc/continuum/consoled.conf";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) clog_set_level(CLOG_DEBUG);
    }

    console_config_t cfg;
    if (console_config_load(config_path, &cfg) != 0) return 1;

    g_store = telemetry_store_create(1024);

    pthread_t http_thread;
    pthread_create(&http_thread, NULL, run_http_server, &cfg.http_listen_port);

    run_telemetry_listener(cfg.telemetry_listen_port); /* blocks forever */

    telemetry_store_destroy(g_store);
    return 0;
}
