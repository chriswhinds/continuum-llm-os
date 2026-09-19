/* api-gatewayd -- ARCH-002 §05 / ARCH-001 §08: the only OpenAI-protocol-
 * aware component in Continuum. Parses POST /v1/chat/completions and
 * GET /v1/models, forwards the request to schedulerd over a Unix socket
 * (protocols/sched_protocol.h), and streams the token response back as
 * either a single JSON body or an SSE stream.
 *
 * Scope: HTTP parsing via the vendored picohttpparser, JSON via the
 * vendored jsmn (both real ARCH-002 §02 dependencies, not
 * reimplementations); one connection handled fully before the next is
 * accepted, matching every other daemon's synchronous reference design.
 * TLS is out of scope here per ARCH-002 §02 -- put a reverse proxy in
 * front for a real deployment. "Tokenization" is the identity function on
 * UTF-8 bytes (see toy_model.h): there's no real subword tokenizer, and a
 * chat request's messages are simply concatenated, not run through a
 * chat template.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "clog.h"
#include "gateway_config.h"
#include "json_util.h"
#include "picohttpparser.h"
#include "sched_protocol.h"
#include "sock_util.h"
#include "wire.h"

#define MAX_REQUEST_SIZE (1024 * 1024)

typedef struct {
    int fd;
    char *buf;
    size_t len;
    size_t cap;
} conn_buf_t;

static void conn_buf_init(conn_buf_t *c, int fd) {
    c->fd = fd;
    c->cap = 8192;
    c->buf = malloc(c->cap);
    c->len = 0;
}

static int conn_buf_read_more(conn_buf_t *c) {
    if (c->len == c->cap) {
        if (c->cap >= MAX_REQUEST_SIZE) return -1;
        c->cap *= 2;
        c->buf = realloc(c->buf, c->cap);
    }
    ssize_t n = read(c->fd, c->buf + c->len, c->cap - c->len);
    if (n <= 0) return -1;
    c->len += (size_t)n;
    return 0;
}

static void write_all_str(int fd, const char *s) { write(fd, s, strlen(s)); }

static void respond_simple(int fd, int status, const char *status_text, const char *content_type,
                            const char *body, size_t body_len) {
    char header[256];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      status, status_text, content_type, body_len);
    write(fd, header, (size_t)n);
    if (body_len) write(fd, body, body_len);
}

static void respond_error(int fd, int status, const char *status_text, const char *message) {
    char body[256];
    int n = snprintf(body, sizeof(body), "{\"error\":{\"message\":\"%s\"}}", message);
    respond_simple(fd, status, status_text, "application/json", body, (size_t)n);
}

static void handle_models(const gateway_config_t *cfg, int fd) {
    char body[2048];
    size_t pos = 0;
    pos += (size_t)snprintf(body + pos, sizeof(body) - pos, "{\"object\":\"list\",\"data\":[");
    for (size_t i = 0; i < cfg->n_models; i++) {
        pos += (size_t)snprintf(body + pos, sizeof(body) - pos,
                                 "%s{\"id\":\"%s\",\"object\":\"model\",\"owned_by\":\"continuum\"}",
                                 i > 0 ? "," : "", cfg->models[i]);
    }
    pos += (size_t)snprintf(body + pos, sizeof(body) - pos, "]}");
    respond_simple(fd, 200, "OK", "application/json", body, pos);
}

typedef struct {
    char model[32];
    char prompt[8192];
    uint32_t max_tokens;
    float temperature;
    int stream;
} chat_request_t;

/* Returns 0 on success, -1 on a malformed body (caller responds 400). */
static int parse_chat_request(const char *body, size_t body_len, chat_request_t *out) {
    memset(out, 0, sizeof(*out));
    out->max_tokens = 64;
    out->temperature = 0.8f;

    jsmn_parser p;
    jsmntok_t tokens[512];
    jsmn_init(&p);
    int n = jsmn_parse(&p, body, body_len, tokens, 512);
    if (n < 1 || tokens[0].type != JSMN_OBJECT) return -1;

    int model_idx = json_object_get(body, tokens, n, 0, "model");
    if (model_idx < 0) return -1;
    json_copy_string(body, &tokens[model_idx], out->model, sizeof(out->model));

    int messages_idx = json_object_get(body, tokens, n, 0, "messages");
    if (messages_idx >= 0 && tokens[messages_idx].type == JSMN_ARRAY) {
        int item = messages_idx + 1;
        size_t prompt_pos = 0;
        for (int i = 0; i < tokens[messages_idx].size; i++) {
            int content_idx = json_object_get(body, tokens, n, item, "content");
            if (content_idx >= 0 && tokens[content_idx].type == JSMN_STRING) {
                char piece[4096];
                json_copy_string(body, &tokens[content_idx], piece, sizeof(piece));
                size_t piece_len = strlen(piece);
                if (prompt_pos + piece_len + 1 < sizeof(out->prompt)) {
                    if (prompt_pos > 0) out->prompt[prompt_pos++] = '\n';
                    memcpy(out->prompt + prompt_pos, piece, piece_len);
                    prompt_pos += piece_len;
                    out->prompt[prompt_pos] = '\0';
                }
            }
            item = json_skip(tokens, item);
        }
    }

    int max_tokens_idx = json_object_get(body, tokens, n, 0, "max_tokens");
    if (max_tokens_idx >= 0) out->max_tokens = (uint32_t)json_to_number(body, &tokens[max_tokens_idx]);
    int temp_idx = json_object_get(body, tokens, n, 0, "temperature");
    if (temp_idx >= 0) out->temperature = (float)json_to_number(body, &tokens[temp_idx]);
    int stream_idx = json_object_get(body, tokens, n, 0, "stream");
    if (stream_idx >= 0) out->stream = json_to_bool(body, &tokens[stream_idx]);

    return out->prompt[0] ? 0 : -1;
}

static int dispatch_to_scheduler(const gateway_config_t *cfg, const chat_request_t *req, uint64_t stream_id) {
    int fd = sock_unix_connect(cfg->scheduler_unix_socket);
    if (fd < 0) return -1;

    size_t prompt_len = strlen(req->prompt);
    size_t reqlen = sizeof(sched_request_header_t) + prompt_len * sizeof(uint32_t);
    uint8_t *buf = malloc(reqlen);
    sched_request_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strncpy(hdr.model_name, req->model, sizeof(hdr.model_name) - 1);
    hdr.n_prompt_tokens = (uint32_t)prompt_len;
    hdr.max_new_tokens = req->max_tokens;
    hdr.temperature = req->temperature;
    hdr.rng_seed = (uint32_t)stream_id ? (uint32_t)stream_id : 1u;
    memcpy(buf, &hdr, sizeof(hdr));
    uint32_t *toks = (uint32_t *)(buf + sizeof(hdr));
    for (size_t i = 0; i < prompt_len; i++) toks[i] = (uint32_t)(unsigned char)req->prompt[i];

    int rc = wire_send_frame_blocking(fd, WIRE_SCHED_REQUEST, 0, stream_id, buf, (uint32_t)reqlen);
    free(buf);
    if (rc != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_chat_completions(const gateway_config_t *cfg, int client_fd, const char *body, size_t body_len) {
    chat_request_t req;
    if (parse_chat_request(body, body_len, &req) != 0) {
        respond_error(client_fd, 400, "Bad Request", "invalid chat completion request");
        return;
    }

    uint64_t stream_id = (uint64_t)(uintptr_t)&req ^ (uint64_t)time(NULL);
    int sched_fd = dispatch_to_scheduler(cfg, &req, stream_id);
    if (sched_fd < 0) {
        respond_error(client_fd, 503, "Service Unavailable", "scheduler unreachable");
        return;
    }

    char id[32];
    snprintf(id, sizeof(id), "chatcmpl-%08x", (unsigned)stream_id);

    if (req.stream) {
        write_all_str(client_fd,
                       "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                       "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
    }

    char *collected = req.stream ? NULL : malloc(65536);
    size_t collected_len = 0;

    for (;;) {
        uint8_t type, flags;
        uint64_t rstream;
        uint8_t chunkbuf[64];
        uint32_t paylen;
        if (wire_recv_frame_blocking(sched_fd, &type, &flags, &rstream, chunkbuf, sizeof(chunkbuf), &paylen) != 0) break;

        if (type == WIRE_ERROR) {
            if (req.stream) {
                write_all_str(client_fd, "data: {\"error\":\"scheduler error\"}\n\ndata: [DONE]\n\n");
            } else {
                respond_error(client_fd, 502, "Bad Gateway", "scheduler reported an error");
                free(collected);
                close(sched_fd);
                return;
            }
            break;
        }
        if (type != WIRE_TOKEN_CHUNK || paylen < 4) break;

        uint32_t token;
        memcpy(&token, chunkbuf, 4);
        char byte = (char)(token & 0xff);
        int is_final = flags & WIRE_FLAG_FINAL;

        if (req.stream) {
            char piece[16];
            size_t plen = 0;
            json_escape_append(piece, sizeof(piece), &plen, &byte, 1);
            piece[plen] = '\0';
            char chunk_json[256];
            int n = snprintf(chunk_json, sizeof(chunk_json),
                              "data: {\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"model\":\"%s\","
                              "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"%s\"},\"finish_reason\":%s}]}\n\n",
                              id, req.model, piece, is_final ? "\"stop\"" : "null");
            write(client_fd, chunk_json, (size_t)n);
        } else if (collected_len + 1 < 65536) {
            collected[collected_len++] = byte;
        }

        if (is_final) break;
    }
    close(sched_fd);

    if (req.stream) {
        write_all_str(client_fd, "data: [DONE]\n\n");
    } else {
        char content[128];
        size_t pos = 0;
        json_escape_append(content, sizeof(content), &pos, collected, collected_len);
        content[pos] = '\0';

        char body_out[512];
        int n = snprintf(body_out, sizeof(body_out),
                          "{\"id\":\"%s\",\"object\":\"chat.completion\",\"model\":\"%s\","
                          "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"%s\"},"
                          "\"finish_reason\":\"stop\"}]}",
                          id, req.model, content);
        respond_simple(client_fd, 200, "OK", "application/json", body_out, (size_t)n);
        free(collected);
    }
}

static void handle_connection(const gateway_config_t *cfg, int fd) {
    conn_buf_t cb;
    conn_buf_init(&cb, fd);

    const char *method, *path;
    size_t method_len, path_len, prev_len = 0;
    int minor_version;
    struct phr_header headers[32];
    size_t n_headers;
    int parsed;

    for (;;) {
        n_headers = 32;
        parsed = phr_parse_request(cb.buf, cb.len, &method, &method_len, &path, &path_len,
                                    &minor_version, headers, &n_headers, prev_len);
        if (parsed > 0) break;
        if (parsed == -1) {
            respond_error(fd, 400, "Bad Request", "malformed HTTP request");
            free(cb.buf);
            return;
        }
        prev_len = cb.len;
        if (conn_buf_read_more(&cb) != 0) {
            free(cb.buf);
            return;
        }
    }

    size_t content_length = 0;
    for (size_t i = 0; i < n_headers; i++) {
        if (headers[i].name_len == 14 && strncasecmp(headers[i].name, "Content-Length", 14) == 0) {
            char lenbuf[32];
            size_t l = headers[i].value_len < sizeof(lenbuf) - 1 ? headers[i].value_len : sizeof(lenbuf) - 1;
            memcpy(lenbuf, headers[i].value, l);
            lenbuf[l] = '\0';
            content_length = (size_t)atol(lenbuf);
        }
    }

    while (cb.len < (size_t)parsed + content_length) {
        if (conn_buf_read_more(&cb) != 0) break;
    }
    const char *body = cb.buf + parsed;
    size_t body_len = cb.len - (size_t)parsed < content_length ? cb.len - (size_t)parsed : content_length;

    char path_buf[256];
    size_t pl = path_len < sizeof(path_buf) - 1 ? path_len : sizeof(path_buf) - 1;
    memcpy(path_buf, path, pl);
    path_buf[pl] = '\0';

    if (method_len == 3 && strncmp(method, "GET", 3) == 0 && strcmp(path_buf, "/v1/models") == 0) {
        handle_models(cfg, fd);
    } else if (method_len == 4 && strncmp(method, "POST", 4) == 0 && strcmp(path_buf, "/v1/chat/completions") == 0) {
        handle_chat_completions(cfg, fd, body, body_len);
    } else {
        respond_error(fd, 404, "Not Found", "no such endpoint");
    }

    free(cb.buf);
}

int main(int argc, char **argv) {
    clog_init("api-gatewayd");
    const char *config_path = "/etc/continuum/api-gatewayd.conf";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) clog_set_level(CLOG_DEBUG);
    }

    gateway_config_t cfg;
    if (gateway_config_load(config_path, &cfg) != 0) return 1;

    int listen_fd = sock_tcp_listen(NULL, cfg.listen_port, 32);
    if (listen_fd < 0) {
        clog_error("api-gatewayd: cannot listen on :%u: %s", cfg.listen_port, strerror(errno));
        return 1;
    }
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags & ~O_NONBLOCK);

    clog_info("api-gatewayd: OpenAI-compatible endpoint ready on :%u (%zu model(s) advertised)",
              cfg.listen_port, cfg.n_models);

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            clog_error("api-gatewayd: accept() failed: %s", strerror(errno));
            break;
        }
        handle_connection(&cfg, fd);
        close(fd);
    }
    return 0;
}
