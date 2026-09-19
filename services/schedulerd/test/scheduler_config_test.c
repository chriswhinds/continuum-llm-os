#include "scheduler_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/scheduler_config_test.conf";
    FILE *f = fopen(path, "w");
    fprintf(f, "listen_unix_socket /tmp/sched.sock\n");
    fprintf(f, "route toy-model 10.0.0.21:7301\n");
    fprintf(f, "route toy-model-2 10.0.0.22:7301\n");
    fclose(f);

    scheduler_config_t cfg;
    assert(scheduler_config_load(path, &cfg) == 0);
    assert(strcmp(cfg.listen_unix_socket, "/tmp/sched.sock") == 0);
    assert(cfg.n_routes == 2);

    const scheduler_route_t *r = scheduler_find_route(&cfg, "toy-model-2");
    assert(r != NULL);
    assert(strcmp(r->shard_execd_host, "10.0.0.22") == 0);
    assert(r->shard_execd_port == 7301);

    assert(scheduler_find_route(&cfg, "nonexistent") == NULL);

    unlink(path);
    printf("all scheduler_config tests passed\n");
    return 0;
}
