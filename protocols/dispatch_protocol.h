/* dispatch_protocol.h -- the payload shape carried inside WIRE_DISPATCH_
 * PROMPT / WIRE_TOKEN_CHUNK frames between schedulerd and shard-execd
 * (ARCH-001 Fig. 4's "dispatch prompt tokens" / "token chunk" steps).
 * Shared so schedulerd doesn't have to guess shard-execd's wire format.
 */
#ifndef CONTINUUM_DISPATCH_PROTOCOL_H
#define CONTINUUM_DISPATCH_PROTOCOL_H

#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t n_prompt_tokens;
    uint32_t max_new_tokens;
    float temperature; /* <=0 means greedy argmax */
    uint32_t rng_seed;
    /* n_prompt_tokens x uint32_t token ids follow this header in the same frame */
} dispatch_prompt_header_t;

typedef struct {
    uint32_t token;
} token_chunk_t;
#pragma pack(pop)

#endif
