/* json_util.h -- small helpers built on the vendored jsmn tokenizer
 * (third_party/jsmn) for pulling the handful of fields api-gatewayd
 * cares about out of an OpenAI-shaped chat completion request body.
 * jsmn only tokenizes -- it doesn't build a tree or offer field lookup,
 * so this file adds just enough of that to read a known-shaped object
 * without a general JSON library. */
#ifndef CONTINUUM_JSON_UTIL_H
#define CONTINUUM_JSON_UTIL_H

#include <stddef.h>

#include "jsmn.h"

/* Returns the index of the token immediately after tokens[i]'s full
 * subtree (itself plus, recursively, every child) -- the building block
 * for walking objects/arrays without a tree structure. */
int json_skip(const jsmntok_t *tokens, int i);

/* Looks up `key` among obj_idx's immediate children (obj_idx must be a
 * JSMN_OBJECT token). Returns the value token's index, or -1 if absent. */
int json_object_get(const char *json, const jsmntok_t *tokens, int n_tokens, int obj_idx, const char *key);

/* Copies a JSMN_STRING token's content into out (NUL-terminated, at most
 * out_cap-1 bytes), unescaping \" \\ \/ \n \r \t \b \f. A \uXXXX escape is
 * copied through literally rather than UTF-8 encoded -- acceptable for
 * this reference build's byte-level "tokenizer" (see toy_model.h), which
 * has no real Unicode handling to feed correctly decoded text into
 * anyway. */
void json_copy_string(const char *json, const jsmntok_t *tok, char *out, size_t out_cap);

double json_to_number(const char *json, const jsmntok_t *tok);
int json_to_bool(const char *json, const jsmntok_t *tok);
int json_streq(const char *json, const jsmntok_t *tok, const char *s);

/* Appends `s` (s_len raw bytes -- may include arbitrary bytes, since this
 * reference build's "tokens" are just bytes, see toy_model.h) to out as a
 * JSON string escape, advancing *pos. Truncates silently at out_cap. */
void json_escape_append(char *out, size_t out_cap, size_t *pos, const char *s, size_t s_len);

#endif
