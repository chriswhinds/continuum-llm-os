/* cpu_load.h -- a tiny /proc/stat-based CPU utilization sampler. Two
 * consecutive reads are needed to compute a percentage (the counters in
 * /proc/stat are cumulative since boot); call cpu_load_sample() once per
 * health-report tick and it maintains the previous snapshot internally. */
#ifndef NODE_AGENT_CPU_LOAD_H
#define NODE_AGENT_CPU_LOAD_H

/* Returns the percentage (0-100) of CPU busy time since the last call, or
 * 0.0 on the very first call (no prior snapshot to diff against) or if
 * /proc/stat is unreadable (e.g. a non-Linux dev sandbox). */
float cpu_load_sample(void);

#endif
