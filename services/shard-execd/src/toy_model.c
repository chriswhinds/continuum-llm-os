#include "toy_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clog.h"

static float *load_tensor(pageclient_t *pc, toymodel_tensor_loc_t loc, size_t numel, uint32_t page_size) {
    uint8_t *raw = malloc((size_t)loc.n_pages * page_size);
    for (uint32_t p = 0; p < loc.n_pages; p++) {
        const void *page = pageclient_get_page(pc, loc.start_page + p);
        if (!page) {
            clog_error("toy_model: failed to fetch page %u while loading a tensor", loc.start_page + p);
            free(raw);
            return NULL;
        }
        memcpy(raw + (size_t)p * page_size, page, page_size);
    }

    float *out = malloc(numel * sizeof(float));
    /* raw bytes are int8; scale applied by the caller, which knows hdr.scale */
    for (size_t i = 0; i < numel; i++) out[i] = (float)((int8_t *)raw)[i];
    free(raw);
    return out;
}

toy_model_t *toy_model_load(const char *weight_file_path, pageclient_t *pc) {
    FILE *f = fopen(weight_file_path, "rb");
    if (!f) {
        clog_error("toy_model: cannot open %s for header read", weight_file_path);
        return NULL;
    }
    toymodel_header_t hdr;
    size_t n = fread(&hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n != sizeof(hdr) || memcmp(hdr.magic, TOYMODEL_MAGIC, sizeof(hdr.magic)) != 0) {
        clog_error("toy_model: %s is not a valid toy model weight file", weight_file_path);
        return NULL;
    }

    toy_model_t *m = calloc(1, sizeof(*m));
    m->hdr = hdr;

#define LOAD(field, numel)                                                          \
    m->field = load_tensor(pc, hdr.field, (numel), hdr.page_size);                  \
    if (!m->field) { toy_model_free(m); return NULL; }                              \
    for (size_t i = 0; i < (size_t)(numel); i++) m->field[i] *= hdr.scale;

    LOAD(embedding, (size_t)hdr.vocab_size * hdr.dim)
    LOAD(w_q, (size_t)hdr.dim * hdr.dim)
    LOAD(w_k, (size_t)hdr.dim * hdr.dim)
    LOAD(w_v, (size_t)hdr.dim * hdr.dim)
    LOAD(w_o, (size_t)hdr.dim * hdr.dim)
    LOAD(w_ff1, (size_t)hdr.dim * hdr.ff_dim)
    LOAD(w_ff2, (size_t)hdr.ff_dim * hdr.dim)
    LOAD(unembedding, (size_t)hdr.dim * hdr.vocab_size)
#undef LOAD

    clog_info("toy_model: loaded vocab=%u dim=%u ff_dim=%u (%zu tensor pages fetched via swapd)",
              hdr.vocab_size, hdr.dim, hdr.ff_dim,
              (size_t)(hdr.embedding.n_pages + hdr.w_q.n_pages + hdr.w_k.n_pages + hdr.w_v.n_pages +
                       hdr.w_o.n_pages + hdr.w_ff1.n_pages + hdr.w_ff2.n_pages + hdr.unembedding.n_pages));
    return m;
}

void toy_model_free(toy_model_t *m) {
    if (!m) return;
    free(m->embedding);
    free(m->w_q);
    free(m->w_k);
    free(m->w_v);
    free(m->w_o);
    free(m->w_ff1);
    free(m->w_ff2);
    free(m->unembedding);
    free(m);
}

kv_session_t *kv_session_create(uint32_t dim, size_t initial_capacity) {
    kv_session_t *s = calloc(1, sizeof(*s));
    s->dim = dim;
    s->capacity = initial_capacity ? initial_capacity : 16;
    s->keys = malloc(s->capacity * dim * sizeof(float));
    s->values = malloc(s->capacity * dim * sizeof(float));
    s->len = 0;
    return s;
}

void kv_session_free(kv_session_t *s) {
    if (!s) return;
    free(s->keys);
    free(s->values);
    free(s);
}

void kv_session_append(kv_session_t *s, const float *key, const float *value) {
    if (s->len == s->capacity) {
        s->capacity *= 2;
        s->keys = realloc(s->keys, s->capacity * s->dim * sizeof(float));
        s->values = realloc(s->values, s->capacity * s->dim * sizeof(float));
    }
    memcpy(s->keys + s->len * s->dim, key, s->dim * sizeof(float));
    memcpy(s->values + s->len * s->dim, value, s->dim * sizeof(float));
    s->len++;
}

/* out[j] = sum_i x[i] * W[i*out_dim + j] -- x is a row vector, W is
 * [in_dim, out_dim] row-major. */
static void matvec(float *out, const float *x, const float *W, uint32_t in_dim, uint32_t out_dim) {
    for (uint32_t j = 0; j < out_dim; j++) {
        float acc = 0.0f;
        for (uint32_t i = 0; i < in_dim; i++) acc += x[i] * W[(size_t)i * out_dim + j];
        out[j] = acc;
    }
}

void toy_model_forward_step(const toy_model_t *m, kv_session_t *sess, uint32_t token, float *out_logits) {
    uint32_t dim = m->hdr.dim;
    const float *x = &m->embedding[(size_t)token * dim];

    float *q = malloc(dim * sizeof(float));
    float *k = malloc(dim * sizeof(float));
    float *v = malloc(dim * sizeof(float));
    matvec(q, x, m->w_q, dim, dim);
    matvec(k, x, m->w_k, dim, dim);
    matvec(v, x, m->w_v, dim, dim);
    kv_session_append(sess, k, v);

    /* Causal scaled dot-product attention over every position up to and
     * including this one (sess now includes this step's k/v). */
    size_t hist = sess->len;
    float *scores = malloc(hist * sizeof(float));
    float scale = 1.0f / sqrtf((float)dim);
    float max_score = -INFINITY;
    for (size_t i = 0; i < hist; i++) {
        float dot = 0.0f;
        const float *ki = sess->keys + i * dim;
        for (uint32_t d = 0; d < dim; d++) dot += q[d] * ki[d];
        scores[i] = dot * scale;
        if (scores[i] > max_score) max_score = scores[i];
    }
    float sum_exp = 0.0f;
    for (size_t i = 0; i < hist; i++) {
        scores[i] = expf(scores[i] - max_score);
        sum_exp += scores[i];
    }
    float *attn_out = calloc(dim, sizeof(float));
    for (size_t i = 0; i < hist; i++) {
        float w = scores[i] / sum_exp;
        const float *vi = sess->values + i * dim;
        for (uint32_t d = 0; d < dim; d++) attn_out[d] += w * vi[d];
    }

    float *o = malloc(dim * sizeof(float));
    matvec(o, attn_out, m->w_o, dim, dim);

    float *x2 = malloc(dim * sizeof(float));
    for (uint32_t d = 0; d < dim; d++) x2[d] = x[d] + o[d];

    float *ff_hidden = malloc(m->hdr.ff_dim * sizeof(float));
    matvec(ff_hidden, x2, m->w_ff1, dim, m->hdr.ff_dim);
    for (uint32_t d = 0; d < m->hdr.ff_dim; d++) {
        if (ff_hidden[d] < 0.0f) ff_hidden[d] = 0.0f; /* ReLU */
    }
    float *ff_out = malloc(dim * sizeof(float));
    matvec(ff_out, ff_hidden, m->w_ff2, m->hdr.ff_dim, dim);

    float *x3 = malloc(dim * sizeof(float));
    for (uint32_t d = 0; d < dim; d++) x3[d] = x2[d] + ff_out[d];

    matvec(out_logits, x3, m->unembedding, dim, m->hdr.vocab_size);

    free(q); free(k); free(v); free(scores); free(attn_out); free(o);
    free(x2); free(ff_hidden); free(ff_out); free(x3);
}

uint32_t toy_model_sample(const float *logits, uint32_t vocab_size, float temperature, uint32_t *rng_state) {
    if (temperature <= 0.0f) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < vocab_size; i++) {
            if (logits[i] > logits[best]) best = i;
        }
        return best;
    }

    float *probs = malloc(vocab_size * sizeof(float));
    float max_logit = logits[0];
    for (uint32_t i = 1; i < vocab_size; i++) {
        if (logits[i] > max_logit) max_logit = logits[i];
    }
    float sum = 0.0f;
    for (uint32_t i = 0; i < vocab_size; i++) {
        probs[i] = expf((logits[i] - max_logit) / temperature);
        sum += probs[i];
    }

    uint32_t x = *rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *rng_state = x;
    float r = ((float)(x % 1000000)) / 1000000.0f * sum;

    float cumulative = 0.0f;
    uint32_t chosen = vocab_size - 1;
    for (uint32_t i = 0; i < vocab_size; i++) {
        cumulative += probs[i];
        if (r <= cumulative) {
            chosen = i;
            break;
        }
    }
    free(probs);
    return chosen;
}
