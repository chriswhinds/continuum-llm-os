/* scheduler_config.h -- config file format for schedulerd.
 *
 * listen_unix_socket /run/continuum/schedulerd.sock
 * route toy-model 10.0.0.21:7301
 * route toy-model-2 10.0.0.22:7301
 *
 * schedulerd's own listener stays a Unix socket -- api-gatewayd and
 * schedulerd are both control-plane services and are assumed colocated
 * on the same node, consistent with ARCH-002 §01. A `route`'s target is
 * a TCP host:port, not a Unix path: shard-execd runs on a *compute*
 * node, a different physical board (ARCH-002 §01), and a Unix socket can
 * never be reached across that boundary. (An earlier version of this
 * file routed to a Unix path, which only ever worked in this repo's own
 * e2e tests because they colocate every daemon in one process tree.)
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
#include <stdint.h>

#define SCHEDULER_MAX_ROUTES 16

typedef struct {
    char model_name[32];
    char shard_execd_host[64];
    uint16_t shard_execd_port;
} scheduler_route_t;

typedef struct {
    char listen_unix_socket[128];
    scheduler_route_t routes[SCHEDULER_MAX_ROUTES];
    size_t n_routes;
} scheduler_config_t;

int scheduler_config_load(const char *path, scheduler_config_t *out);
const scheduler_route_t *scheduler_find_route(const scheduler_config_t *cfg, const char *model_name);

#endif
