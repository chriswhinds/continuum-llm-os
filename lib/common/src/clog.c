#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "clog.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

static char g_tag[32] = "continuum";
static clog_level_t g_min_level = CLOG_INFO;

static const char *level_name(clog_level_t l) {
    switch (l) {
        case CLOG_DEBUG: return "DEBUG";
        case CLOG_INFO:  return "INFO ";
        case CLOG_WARN:  return "WARN ";
        case CLOG_ERROR: return "ERROR";
    }
    return "?????";
}

void clog_init(const char *tag) {
    if (!tag) return;
    strncpy(g_tag, tag, sizeof(g_tag) - 1);
    g_tag[sizeof(g_tag) - 1] = '\0';
}

void clog_set_level(clog_level_t level) {
    g_min_level = level;
}

void clog_log(clog_level_t level, const char *fmt, ...) {
    if (level < g_min_level) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_buf);

    fprintf(stderr, "%s.%03ld [%s] %-9s pid=%d ", timebuf, ts.tv_nsec / 1000000,
            level_name(level), g_tag, (int)getpid());

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}
