/* sched_protocol.h -- the payload carried inside WIRE_SCHED_REQUEST, from
 * api-gatewayd to schedulerd (ARCH-001 Fig. 4's "route(model, priority,
 * est. tokens)" step). schedulerd strips `model_name` (used only for
 * placement) and forwards the rest to the chosen shard-execd as a
 * dispatch_prompt_header_t (see protocols/dispatch_protocol.h) -- the two
 * structs are deliberately shaped the same way past the model name so
 * that forwarding is a straight byte-range copy, not a re-encode.
 */
#ifndef CONTINUUM_SCHED_PROTOCOL_H
#define CONTINUUM_SCHED_PROTOCOL_H

#include <stdint.h>

#define SCHED_MODEL_NAME_MAX 32

#pragma pack(push, 1)
typedef struct {
    char model_name[SCHED_MODEL_NAME_MAX];
    uint32_t n_prompt_tokens;
    uint32_t max_new_tokens;
    float temperature;
    uint32_t rng_seed;
    /* n_prompt_tokens x uint32_t token ids follow this header in the same frame */
} sched_request_header_t;
#pragma pack(pop)

#endif
