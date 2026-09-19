/* gateway_config.h -- config file format for api-gatewayd.
 *
 * listen_port 8080
 * scheduler_unix_socket /run/continuum/schedulerd.sock
 * model toy-model
 * model toy-model-2
 *
 * Scope note: the `model` list is only for answering GET /v1/models --
 * it's a static duplicate of schedulerd's own route table (scheduler_
 * config.h) rather than a live query, so the two configs must be kept in
 * sync by whoever deploys them. A non-reference build would have the
 * gateway ask the scheduler (or the page directory) for the live model
 * list instead of hand-maintaining it twice.
 */
#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define GATEWAY_MAX_MODELS 16

typedef struct {
    uint16_t listen_port;
    char scheduler_unix_socket[128];
    char models[GATEWAY_MAX_MODELS][32];
    size_t n_models;
} gateway_config_t;

int gateway_config_load(const char *path, gateway_config_t *out);

#endif
