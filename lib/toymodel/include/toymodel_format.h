/* toymodel_format.h -- the weight file layout shared by tools/gen_toy_
 * weights (which writes one) and shard-execd (which reads one).
 *
 * Scope note: ARCH-002 §02 names a safetensors-style format as the real
 * target. This reference build uses a much smaller custom format instead
 * -- a fixed header plus page-aligned raw int8 tensors -- because a full
 * safetensors reader/writer is a lot of parsing machinery in service of a
 * toy single-layer model that has nothing else in common with a real
 * checkpoint. The part of the architecture this build exists to prove --
 * demand-paging tensor bytes through swapd's tiered resolution -- doesn't
 * care what the header parser looks like, only that tensors land on page
 * boundaries, which this format does too.
 *
 * The model itself: byte-level vocabulary (256 tokens == raw bytes, so
 * there's no separate tokenizer), one causal self-attention block, one
 * FFN block, tied to nothing (separate embedding and unembedding
 * matrices). Every weight is int8 with a single shared dequantization
 * scale (float_value = int8_value * header.scale) -- real quantized
 * inference uses per-channel scales for accuracy; one global scale is a
 * further deliberate simplification of an already-toy model.
 *
 * Every tensor starts on its own page boundary. header occupies page 0
 * in its entirety (read directly from the file by whoever loads it --
 * it's metadata, not part of the demand-paged tensor region). Every
 * other tensor's `start_page` is an absolute page index into the same
 * file, which is exactly the page index swapd's paging_engine uses for
 * its pread()s -- so no translation is needed between "which page holds
 * tensor X" (this header) and "fetch page N" (the wire protocol).
 */
#ifndef CONTINUUM_TOYMODEL_FORMAT_H
#define CONTINUUM_TOYMODEL_FORMAT_H

#include <stdint.h>

#define TOYMODEL_MAGIC "CTMDLv1\0"

typedef struct {
    uint32_t start_page;
    uint32_t n_pages;
} toymodel_tensor_loc_t;

#pragma pack(push, 1)
typedef struct {
    char magic[8];
    uint32_t vocab_size;
    uint32_t dim;
    uint32_t ff_dim;
    uint32_t page_size;
    float scale;
    uint32_t reserved;

    toymodel_tensor_loc_t embedding;   /* [vocab_size, dim] */
    toymodel_tensor_loc_t w_q;         /* [dim, dim] */
    toymodel_tensor_loc_t w_k;         /* [dim, dim] */
    toymodel_tensor_loc_t w_v;         /* [dim, dim] */
    toymodel_tensor_loc_t w_o;         /* [dim, dim] */
    toymodel_tensor_loc_t w_ff1;       /* [dim, ff_dim] */
    toymodel_tensor_loc_t w_ff2;       /* [ff_dim, dim] */
    toymodel_tensor_loc_t unembedding; /* [dim, vocab_size] */
} toymodel_header_t;
#pragma pack(pop)

static inline uint32_t toymodel_pages_for_bytes(uint64_t bytes, uint32_t page_size) {
    return (uint32_t)((bytes + page_size - 1) / page_size);
}

#endif
