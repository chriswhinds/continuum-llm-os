#include "node_agent_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/node_agentd_test.conf";
    FILE *f = fopen(path, "w");
    fprintf(f, "node_id 7\n");
    fprintf(f, "listen_port 7207\n");
    fprintf(f, "page_size 4096\n");
    fprintf(f, "health_interval_ms 500\n");
    fprintf(f, "telemetry_sink 10.0.0.10:7401\n");
    fprintf(f, "telemetry_sink 10.0.0.11:7401\n");
    fclose(f);

    node_agent_config_t cfg;
    assert(node_agent_config_load(path, &cfg) == 0);
    assert(cfg.node_id == 7);
    assert(cfg.listen_port == 7207);
    assert(cfg.page_size == 4096);
    assert(cfg.health_interval_ms == 500);
    assert(cfg.n_sinks == 2);
    assert(strcmp(cfg.sinks[0].host, "10.0.0.10") == 0);
    assert(cfg.sinks[0].port == 7401);
    assert(strcmp(cfg.sinks[1].host, "10.0.0.11") == 0);

    unlink(path);
    printf("all node_agent_config tests passed\n");
    return 0;
}
