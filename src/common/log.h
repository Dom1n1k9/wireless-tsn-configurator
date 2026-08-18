#ifndef WTSN_LOG_H
#define WTSN_LOG_H

#include <stdarg.h>

typedef enum {
    WTSN_LOG_DEBUG = 0,
    WTSN_LOG_INFO,
    WTSN_LOG_WARN,
    WTSN_LOG_ERROR
} wtsn_log_level;

void wtsn_log_init(wtsn_log_level level, const char *file);
void wtsn_log_to_file(const char *path);
void wtsn_log(wtsn_log_level level, const char *fmt, ...);
void wtsn_log_set_level(wtsn_log_level level);

#endif
