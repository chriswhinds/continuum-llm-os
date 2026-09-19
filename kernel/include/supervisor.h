/* supervisor.h -- continuumd's process supervision: fork/exec each
 * configured service as its own child (fault isolation -- a crash in one
 * service must not take another down, ARCH-002 §04), reap exits via the
 * caller's SIGCHLD handling, and respawn with exponential backoff. */
#ifndef CONTINUUMD_SUPERVISOR_H
#define CONTINUUMD_SUPERVISOR_H

#include <stdint.h>
#include <sys/types.h>

#include "config.h"

typedef struct {
    const service_spec_t *spec;
    pid_t pid;      /* 0 if not currently running */
    int restart_count;
    uint64_t next_restart_at_ms; /* CLOCK_MONOTONIC ms; valid only if pid==0 */
} child_state_t;

typedef struct {
    child_state_t children[CONTINUUMD_MAX_SERVICES];
    size_t n_children;
} supervisor_t;

void supervisor_init(supervisor_t *sup, const continuumd_config_t *cfg);

/* fork()+exec()s every configured child immediately (startup). */
void supervisor_start_all(supervisor_t *sup);

/* Call once per reaped SIGCHLD (i.e. inside a waitpid(-1, &status, WNOHANG)
 * loop) for every pid that exited, so the supervisor can mark it dead and
 * schedule a respawn. */
void supervisor_on_child_exit(supervisor_t *sup, pid_t pid, int status);

/* Call periodically (e.g. every ~500ms from the main loop) to respawn any
 * child whose backoff has elapsed. */
void supervisor_tick(supervisor_t *sup, uint64_t now_ms);

/* Sends `sig` to every currently-running child (used for graceful shutdown). */
void supervisor_signal_all(supervisor_t *sup, int sig);

uint64_t supervisor_now_ms(void);

#endif
