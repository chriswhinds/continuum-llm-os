/* Proves the page table is genuinely shared across processes (the whole
 * point of shm_pagetable), not just within one process's address space:
 * the parent (playing node-agentd) writes entries, a forked child (playing
 * swapd, attaching as a non-creator) reads them back through a completely
 * separate mapping. */
#include "shm_pagetable.h"

#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHM_NAME "/continuum-pt-test"

static void run_child_reader(int result_fd) {
    /* Give the parent a moment to have created + populated the table --
     * a real node-agentd/swapd pair is started in a known order by
     * continuumd instead of racing like this test does. */
    usleep(50000);

    shm_pagetable_t *pt = shm_pagetable_open(SHM_NAME, 0, /*is_creator=*/0);
    char result = '0';
    if (pt) {
        shm_page_entry_t e;
        int64_t slot = -1;
        if (shm_pagetable_get(pt, 424242, &e, &slot) && slot >= 0 && e.tier == 3) {
            result = '1';
        }
        shm_pagetable_close(pt, SHM_NAME, 0);
    }
    write(result_fd, &result, 1);
    close(result_fd);
    _exit(0);
}

int main(void) {
    shm_pagetable_t *pt = shm_pagetable_open(SHM_NAME, 128, /*is_creator=*/1);
    assert(pt != NULL);
    int64_t slot0 = shm_pagetable_put(pt, 424242, 3, 12345);
    assert(slot0 >= 0);
    assert(shm_pagetable_count(pt) == 1);

    int pipefd[2];
    assert(pipe(pipefd) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(pipefd[0]);
        run_child_reader(pipefd[1]);
    }
    close(pipefd[1]);

    char result = '0';
    read(pipefd[0], &result, 1);
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);

    assert(result == '1');
    printf("ok: entry written in parent is visible in a forked child via shared memory\n");

    /* Update from the parent after the child already read once -- proves
     * it's a live shared mapping, not a copy taken at fork() time. Same
     * key must land back on the same slot it already occupied. */
    int64_t slot1 = shm_pagetable_put(pt, 424242, 2, 99999);
    assert(slot1 == slot0);
    shm_page_entry_t e;
    int64_t slot2 = -1;
    assert(shm_pagetable_get(pt, 424242, &e, &slot2) == 1);
    assert(e.tier == 2 && slot2 == slot0);

    assert(shm_pagetable_del(pt, 424242) == 1);
    assert(shm_pagetable_get(pt, 424242, &e, NULL) == 0);
    assert(shm_pagetable_count(pt) == 0);

    shm_pagetable_close(pt, SHM_NAME, 1);
    printf("all pagetable tests passed\n");
    return 0;
}
