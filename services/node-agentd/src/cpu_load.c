#include "cpu_load.h"

#include <stdio.h>

static unsigned long long g_prev_idle, g_prev_total;
static int g_have_prev;

float cpu_load_sample(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0.0f;

    unsigned long long user, nice_, system_, idle, iowait, irq, softirq, steal;
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice_, &system_, &idle, &iowait, &irq, &softirq, &steal);
    fclose(f);
    if (n < 8) return 0.0f;

    unsigned long long idle_all = idle + iowait;
    unsigned long long total = user + nice_ + system_ + idle_all + irq + softirq + steal;

    float pct = 0.0f;
    if (g_have_prev && total > g_prev_total) {
        unsigned long long total_delta = total - g_prev_total;
        unsigned long long idle_delta = idle_all - g_prev_idle;
        if (idle_delta <= total_delta) {
            pct = 100.0f * (float)(total_delta - idle_delta) / (float)total_delta;
        }
    }
    g_prev_idle = idle_all;
    g_prev_total = total;
    g_have_prev = 1;
    return pct;
}
