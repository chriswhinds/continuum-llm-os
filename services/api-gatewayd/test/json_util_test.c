#include "json_util.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *json = "{\"model\":\"toy-model\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\\nthere\"}],"
                        "\"max_tokens\":10,\"temperature\":0.5,\"stream\":true}";
    jsmn_parser p;
    jsmntok_t tokens[64];
    jsmn_init(&p);
    int n = jsmn_parse(&p, json, strlen(json), tokens, 64);
    assert(n > 0);
    assert(tokens[0].type == JSMN_OBJECT);

    int model_idx = json_object_get(json, tokens, n, 0, "model");
    assert(model_idx > 0);
    char model[32];
    json_copy_string(json, &tokens[model_idx], model, sizeof(model));
    assert(strcmp(model, "toy-model") == 0);
    printf("ok: json_object_get + json_copy_string extract a top-level string field\n");

    int messages_idx = json_object_get(json, tokens, n, 0, "messages");
    assert(messages_idx > 0 && tokens[messages_idx].type == JSMN_ARRAY);
    int first_msg = messages_idx + 1;
    int content_idx = json_object_get(json, tokens, n, first_msg, "content");
    assert(content_idx > 0);
    char content[64];
    json_copy_string(json, &tokens[content_idx], content, sizeof(content));
    assert(strcmp(content, "hi\nthere") == 0); /* \n escape must be unescaped */
    printf("ok: nested array/object traversal and escape decoding work\n");

    int max_tokens_idx = json_object_get(json, tokens, n, 0, "max_tokens");
    assert((int)json_to_number(json, &tokens[max_tokens_idx]) == 10);
    int temp_idx = json_object_get(json, tokens, n, 0, "temperature");
    assert(json_to_number(json, &tokens[temp_idx]) == 0.5);
    int stream_idx = json_object_get(json, tokens, n, 0, "stream");
    assert(json_to_bool(json, &tokens[stream_idx]) == 1);
    printf("ok: numeric and boolean field extraction\n");

    assert(json_object_get(json, tokens, n, 0, "nonexistent") == -1);
    printf("ok: missing field reports -1, not a crash\n");

    char out[32];
    size_t pos = 0;
    const char raw[] = {'a', '"', '\\', '\n', (char)0x01};
    json_escape_append(out, sizeof(out), &pos, raw, sizeof(raw));
    out[pos] = '\0';
    assert(strcmp(out, "a\\\"\\\\\\n\\u0001") == 0);
    printf("ok: json_escape_append handles quotes, backslashes, newlines, and control bytes\n");

    printf("all json_util tests passed\n");
    return 0;
}
