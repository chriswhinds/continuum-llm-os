/* scheduler_config.h -- config file format for schedulerd.
 *
 * listen_unix_socket /run/continuum/schedulerd.sock
 * route toy-model /run/continuum/shard-execd.sock
 * route toy-model-2 /run/continuum/shard-execd-2.sock
 *
 * Scope note: each `route` maps a model name to exactly one shard-execd
 * endpoint. ARCH-002 §05's schedulerd batches concurrent decode steps
 * across many requests and many candidate nodes per model; this reference
 * build's placement logic is the degenerate single-candidate case of
 * that -- the part being proven here is the request-routing and
 * streaming-relay plumbing, not a load balancer with nothing yet to
 * balance across in a one-shard-per-model demo.
 */
#ifndef SCHEDULER_CONFIG_H
#define SCHEDULER_CONFIG_H

#include <stddef.h>

#define SCHEDULER_MAX_ROUTES 16

typedef struct {
    char model_name[32];
    char shard_execd_socket[128];
} scheduler_route_t;

typedef struct {
    char listen_unix_socket[128];
    scheduler_route_t routes[SCHEDULER_MAX_ROUTES];
    size_t n_routes;
} scheduler_config_t;

int scheduler_config_load(const char *path, scheduler_config_t *out);
const scheduler_route_t *scheduler_find_route(const scheduler_config_t *cfg, const char *model_name);

#endif
