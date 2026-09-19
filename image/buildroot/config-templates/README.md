# Config templates

These are examples, not what ships in an image (see `post-build.sh`'s
comment on why node-specific config is never baked in). Copy the set
matching a node's role into `/etc/continuum/` on that node and fill in
its `node_id`, peer IPs, and paths before first boot.

- `compute-node/` — the four configs a compute node runs: `continuumd.conf`
  (supervises `node-agentd`, `swapd`, `shard-execd`), plus each service's
  own file. Matches ARCH-002 §01's compute-node role.
- `control-plane-node/` — `continuumd.conf` (supervises `page-directoryd`,
  `membershipd`, `schedulerd`, `api-gatewayd`, `consoled`), plus each
  service's own file, `peer` lines filled in for a 3-node quorum.

Every `service` / `route` / `peer` line's exact syntax is documented in
that service's own `*_config.c` (e.g. `services/swapd/src/swapd_config.c`)
-- these templates are working examples of that syntax, not the syntax
reference itself.
