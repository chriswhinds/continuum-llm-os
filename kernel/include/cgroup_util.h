/* cgroup_util.h -- best-effort cgroup v2 placement for supervised children.
 *
 * This is deliberately best-effort: it requires cgroup v2 to be mounted
 * and delegated to continuumd (true on the target Buildroot image per
 * ARCH-002 §06, not guaranteed on an arbitrary dev/CI machine). Every
 * failure here is logged and swallowed rather than treated as fatal --
 * losing resource isolation is not a reason to refuse to run a service. */
#ifndef CONTINUUMD_CGROUP_UTIL_H
#define CONTINUUMD_CGROUP_UTIL_H

/* Creates /sys/fs/cgroup/continuum/<name> if needed and moves the calling
 * process (expected to be called from the freshly-forked child, before
 * exec) into it. Returns 0 on success, -1 on any failure (logged). */
int cgroup_join_self(const char *name);

#endif
