#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "supervisor.h"

static void test_config_parsing(void) {
    const char *path = "/tmp/continuumd_test.conf";
    FILE *f = fopen(path, "w");
    fprintf(f, "# a comment\n");
    fprintf(f, "uart_device /dev/ttyACM0\n");
    fprintf(f, "heartbeat_interval_ms 1500\n");
    fprintf(f, "service node-agentd /bin/sh -c hello\n");
    fprintf(f, "\n");
    fprintf(f, "service swapd /bin/sh -c world\n");
    fclose(f);

    continuumd_config_t cfg;
    assert(config_load(path, &cfg) == 0);
    assert(strcmp(cfg.uart_device, "/dev/ttyACM0") == 0);
    assert(cfg.heartbeat_interval_ms == 1500);
    assert(cfg.n_services == 2);
    assert(strcmp(cfg.services[0].name, "node-agentd") == 0);
    assert(strcmp(cfg.services[0].exec, "/bin/sh") == 0);
    assert(cfg.services[0].argc == 3); /* /bin/sh, -c, hello */
    assert(strcmp(cfg.services[0].argv[2], "hello") == 0);
    assert(cfg.services[0].argv[3] == NULL);

    unlink(path);
    printf("ok: config parsing\n");
}

static void test_respawn_with_backoff(void) {
    continuumd_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.n_services = 1;
    service_spec_t *svc = &cfg.services[0];
    strcpy(svc->name, "quick-exit");
    strcpy(svc->exec, "/bin/sh");
    svc->argv[0] = svc->exec;
    svc->argv[1] = "-c";
    svc->argv[2] = "exit 3";
    svc->argv[3] = NULL;
    svc->argc = 3;

    supervisor_t sup;
    supervisor_init(&sup, &cfg);
    supervisor_start_all(&sup);

    pid_t first_pid = sup.children[0].pid;
    assert(first_pid > 0);

    int status;
    pid_t reaped = waitpid(first_pid, &status, 0);
    assert(reaped == first_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 3);

    supervisor_on_child_exit(&sup, reaped, status);
    assert(sup.children[0].pid == 0);
    assert(sup.children[0].restart_count == 1);
    assert(sup.children[0].next_restart_at_ms > supervisor_now_ms()); /* backoff not yet elapsed */

    /* Before the backoff elapses, a tick must NOT respawn. */
    supervisor_tick(&sup, supervisor_now_ms());
    assert(sup.children[0].pid == 0);

    /* After the backoff elapses, a tick must respawn. */
    supervisor_tick(&sup, sup.children[0].next_restart_at_ms + 1);
    assert(sup.children[0].pid > 0);
    waitpid(sup.children[0].pid, &status, 0);
    supervisor_on_child_exit(&sup, sup.children[0].pid, status);
    assert(sup.children[0].restart_count == 2);

    printf("ok: respawn honors exponential backoff\n");
}

static void test_signal_all_terminates_long_runner(void) {
    continuumd_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.n_services = 1;
    service_spec_t *svc = &cfg.services[0];
    strcpy(svc->name, "long-runner");
    strcpy(svc->exec, "/bin/sh");
    svc->argv[0] = svc->exec;
    svc->argv[1] = "-c";
    svc->argv[2] = "sleep 30";
    svc->argv[3] = NULL;
    svc->argc = 3;

    supervisor_t sup;
    supervisor_init(&sup, &cfg);
    supervisor_start_all(&sup);
    pid_t pid = sup.children[0].pid;
    assert(pid > 0);

    supervisor_signal_all(&sup, SIGTERM);
    int status;
    pid_t reaped = waitpid(pid, &status, 0);
    assert(reaped == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    printf("ok: supervisor_signal_all terminates a running child\n");
}

int main(void) {
    test_config_parsing();
    test_respawn_with_backoff();
    test_signal_all_terminates_long_runner();
    printf("all supervisor tests passed\n");
    return 0;
}
