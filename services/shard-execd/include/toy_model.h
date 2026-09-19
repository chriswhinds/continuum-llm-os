/* toy_model.h -- loads a toymodel_format.h weight file's tensors through a
 * pageclient (so loading genuinely exercises swapd's demand paging, one
 * page at a time, per tensor) and runs the forward pass: embed -> single
 * causal self-attention block -> FFN -> unembed -> logits. See
 * toymodel_format.h for why this is a toy rather than a real checkpoint
 * format/architecture.
 */
#ifndef CONTINUUM_TOY_MODEL_H
#define CONTINUUM_TOY_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "pageclient.h"
#include "toymodel_format.h"

typedef struct {
    toymodel_header_t hdr;
    float *embedding;   /* [vocab, dim] */
    float *w_q, *w_k, *w_v, *w_o; /* [dim, dim] each */
    float *w_ff1;       /* [dim, ff_dim] */
    float *w_ff2;       /* [ff_dim, dim] */
    float *unembedding; /* [dim, vocab] */
} toy_model_t;

/* Reads the header directly from weight_file_path (metadata, not paged --
 * see toymodel_format.h), then fetches every tensor's bytes through `pc`
 * page by page, dequantizing into this process's own memory. Returns NULL
 * on failure (bad header, or a page swapd couldn't supply). */
toy_model_t *toy_model_load(const char *weight_file_path, pageclient_t *pc);
void toy_model_free(toy_model_t *m);

/* A single sequence's causal self-attention K/V history -- kept in
 * shard-execd's own process memory rather than routed through swapd's
 * paged tensor region (see shard-execd's module comment for why: real
 * tier-aware KV swapping per ARCH-001 §09 is future work built the same
 * way weight paging already is). Grows as tokens are generated. */
typedef struct {
    float *keys;   /* [capacity, dim] */
    float *values; /* [capacity, dim] */
    size_t len;
    size_t capacity;
    uint32_t dim;
} kv_session_t;

kv_session_t *kv_session_create(uint32_t dim, size_t initial_capacity);
void kv_session_free(kv_session_t *s);
void kv_session_append(kv_session_t *s, const float *key, const float *value);

/* Runs one forward step for `token` given the running KV history in
 * `sess` (appending this step's K/V to it), writing `hdr.vocab_size`
 * logits into `out_logits` (caller-allocated). */
void toy_model_forward_step(const toy_model_t *m, kv_session_t *sess, uint32_t token, float *out_logits);

/* Samples a token id from logits using temperature-scaled softmax; temp<=0
 * means greedy argmax. rng_state is xorshift32 state, updated in place. */
uint32_t toy_model_sample(const float *logits, uint32_t vocab_size, float temperature, uint32_t *rng_state);

#endif
