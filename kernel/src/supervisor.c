#include "supervisor.h"

#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cgroup_util.h"
#include "clog.h"

#define BACKOFF_BASE_MS 500
#define BACKOFF_MAX_MS 30000

uint64_t supervisor_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void supervisor_init(supervisor_t *sup, const continuumd_config_t *cfg) {
    memset(sup, 0, sizeof(*sup));
    sup->n_children = cfg->n_services;
    for (size_t i = 0; i < cfg->n_services; i++) {
        sup->children[i].spec = &cfg->services[i];
        sup->children[i].pid = 0;
        sup->children[i].restart_count = 0;
        sup->children[i].next_restart_at_ms = 0;
    }
}

static void spawn_one(child_state_t *cs) {
    pid_t pid = fork();
    if (pid < 0) {
        clog_error("supervisor: fork() failed for %s", cs->spec->name);
        cs->next_restart_at_ms = supervisor_now_ms() + BACKOFF_BASE_MS;
        return;
    }
    if (pid == 0) {
        /* child */
        cgroup_join_self(cs->spec->name); /* best effort, see cgroup_util.h */
        execv(cs->spec->exec, cs->spec->argv);
        /* execv only returns on failure */
        clog_error("supervisor: execv(%s) failed", cs->spec->exec);
        _exit(127);
    }
    cs->pid = pid;
    clog_info("supervisor: started %s (pid %d)", cs->spec->name, pid);
}

void supervisor_start_all(supervisor_t *sup) {
    for (size_t i = 0; i < sup->n_children; i++) {
        spawn_one(&sup->children[i]);
    }
}

void supervisor_on_child_exit(supervisor_t *sup, pid_t pid, int status) {
    for (size_t i = 0; i < sup->n_children; i++) {
        child_state_t *cs = &sup->children[i];
        if (cs->pid != pid) continue;

        if (WIFEXITED(status)) {
            clog_warn("supervisor: %s (pid %d) exited with status %d", cs->spec->name, pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            clog_warn("supervisor: %s (pid %d) killed by signal %d", cs->spec->name, pid, WTERMSIG(status));
        }

        cs->pid = 0;
        int backoff = BACKOFF_BASE_MS << (cs->restart_count < 6 ? cs->restart_count : 6);
        if (backoff > BACKOFF_MAX_MS) backoff = BACKOFF_MAX_MS;
        cs->restart_count++;
        cs->next_restart_at_ms = supervisor_now_ms() + (uint64_t)backoff;
        clog_info("supervisor: %s will restart in %dms (attempt %d)", cs->spec->name, backoff, cs->restart_count);
        return;
    }
}

void supervisor_tick(supervisor_t *sup, uint64_t now_ms) {
    for (size_t i = 0; i < sup->n_children; i++) {
        child_state_t *cs = &sup->children[i];
        if (cs->pid == 0 && now_ms >= cs->next_restart_at_ms) {
            spawn_one(cs);
        }
    }
}

void supervisor_signal_all(supervisor_t *sup, int sig) {
    for (size_t i = 0; i < sup->n_children; i++) {
        if (sup->children[i].pid > 0) kill(sup->children[i].pid, sig);
    }
}
