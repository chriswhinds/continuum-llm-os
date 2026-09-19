#include "json_util.h"

#include <stdlib.h>
#include <string.h>

int json_skip(const jsmntok_t *tokens, int i) {
    /* jsmn gives a STRING token used as an object key size=1 -- "one value
     * follows" -- which is bookkeeping for the parser, not a nested
     * structure to recurse into. Treating it as one here (as an earlier
     * version of this function did) double-counts: it walks into the
     * value via the key's own size *and* the object loop advances past
     * that value again, overshooting into the next key entirely. So a
     * string or primitive is always a leaf for skip purposes, regardless
     * of its `size` field; only OBJECT and ARRAY actually recurse. */
    switch (tokens[i].type) {
        case JSMN_OBJECT: {
            int end = i + 1;
            for (int c = 0; c < tokens[i].size; c++) {
                end += 1;                  /* the key itself: exactly one token */
                end = json_skip(tokens, end); /* the value: may be nested */
            }
            return end;
        }
        case JSMN_ARRAY: {
            int end = i + 1;
            for (int c = 0; c < tokens[i].size; c++) {
                end = json_skip(tokens, end);
            }
            return end;
        }
        default:
            return i + 1;
    }
}

int json_object_get(const char *json, const jsmntok_t *tokens, int n_tokens, int obj_idx, const char *key) {
    if (tokens[obj_idx].type != JSMN_OBJECT) return -1;
    int i = obj_idx + 1;
    for (int entry = 0; entry < tokens[obj_idx].size && i < n_tokens; entry++) {
        const jsmntok_t *k = &tokens[i];
        int value_idx = i + 1;
        if (k->type == JSMN_STRING && json_streq(json, k, key)) {
            return value_idx;
        }
        i = json_skip(tokens, value_idx);
    }
    return -1;
}

int json_streq(const char *json, const jsmntok_t *tok, const char *s) {
    if (tok->type != JSMN_STRING) return 0;
    size_t len = (size_t)(tok->end - tok->start);
    return strlen(s) == len && strncmp(json + tok->start, s, len) == 0;
}

void json_copy_string(const char *json, const jsmntok_t *tok, char *out, size_t out_cap) {
    const char *p = json + tok->start;
    const char *end = json + tok->end;
    size_t o = 0;
    while (p < end && o + 1 < out_cap) {
        if (*p == '\\' && p + 1 < end) {
            char esc = p[1];
            switch (esc) {
                case '"': out[o++] = '"'; p += 2; break;
                case '\\': out[o++] = '\\'; p += 2; break;
                case '/': out[o++] = '/'; p += 2; break;
                case 'n': out[o++] = '\n'; p += 2; break;
                case 'r': out[o++] = '\r'; p += 2; break;
                case 't': out[o++] = '\t'; p += 2; break;
                case 'b': out[o++] = '\b'; p += 2; break;
                case 'f': out[o++] = '\f'; p += 2; break;
                default: out[o++] = *p++; break; /* includes \u, copied through literally -- see header */
            }
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}

double json_to_number(const char *json, const jsmntok_t *tok) {
    char buf[64];
    size_t len = (size_t)(tok->end - tok->start);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, json + tok->start, len);
    buf[len] = '\0';
    return atof(buf);
}

int json_to_bool(const char *json, const jsmntok_t *tok) {
    size_t len = (size_t)(tok->end - tok->start);
    return len == 4 && strncmp(json + tok->start, "true", 4) == 0;
}

void json_escape_append(char *out, size_t out_cap, size_t *pos, const char *s, size_t s_len) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < s_len && *pos + 6 < out_cap; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': out[(*pos)++] = '\\'; out[(*pos)++] = '"'; break;
            case '\\': out[(*pos)++] = '\\'; out[(*pos)++] = '\\'; break;
            case '\n': out[(*pos)++] = '\\'; out[(*pos)++] = 'n'; break;
            case '\r': out[(*pos)++] = '\\'; out[(*pos)++] = 'r'; break;
            case '\t': out[(*pos)++] = '\\'; out[(*pos)++] = 't'; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    out[(*pos)++] = '\\';
                    out[(*pos)++] = 'u';
                    out[(*pos)++] = '0';
                    out[(*pos)++] = '0';
                    out[(*pos)++] = hex[(c >> 4) & 0xf];
                    out[(*pos)++] = hex[c & 0xf];
                } else {
                    out[(*pos)++] = (char)c;
                }
        }
    }
}
