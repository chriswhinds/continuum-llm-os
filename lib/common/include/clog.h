/* clog.h -- minimal leveled logging to stderr, shared by every Continuum daemon. */
#ifndef CONTINUUM_CLOG_H
#define CONTINUUM_CLOG_H

#include <stdarg.h>

typedef enum {
    CLOG_DEBUG = 0,
    CLOG_INFO  = 1,
    CLOG_WARN  = 2,
    CLOG_ERROR = 3,
} clog_level_t;

/* Sets the process-wide tag printed on every line, e.g. "node-agentd". */
void clog_init(const char *tag);

/* Raises/lowers the minimum level that gets printed. Default: CLOG_INFO. */
void clog_set_level(clog_level_t level);

void clog_log(clog_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define clog_debug(...) clog_log(CLOG_DEBUG, __VA_ARGS__)
#define clog_info(...)  clog_log(CLOG_INFO,  __VA_ARGS__)
#define clog_warn(...)  clog_log(CLOG_WARN,  __VA_ARGS__)
#define clog_error(...) clog_log(CLOG_ERROR, __VA_ARGS__)

#endif
