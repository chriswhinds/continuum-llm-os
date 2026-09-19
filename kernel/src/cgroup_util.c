#include "cgroup_util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "clog.h"

int cgroup_join_self(const char *name) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/sys/fs/cgroup/continuum/%s", name);

    /* mkdir -p the two levels we might be missing; ENOENT on the parent
     * (cgroup v2 not mounted/delegated at all) is the common dev-machine
     * case and is not worth two separate log lines. */
    if (mkdir("/sys/fs/cgroup/continuum", 0755) != 0 && errno != EEXIST && errno != ENOENT) {
        clog_warn("cgroup: mkdir /sys/fs/cgroup/continuum failed: %s", strerror(errno));
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        clog_warn("cgroup: %s unavailable (%s) -- running %s without resource limits", dir, strerror(errno), name);
        return -1;
    }

    char procs_path[300];
    snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", dir);
    FILE *f = fopen(procs_path, "w");
    if (!f) {
        clog_warn("cgroup: cannot open %s (%s) -- running %s without resource limits", procs_path, strerror(errno), name);
        return -1;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}
