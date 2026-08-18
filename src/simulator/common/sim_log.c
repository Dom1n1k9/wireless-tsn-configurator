#include "simulator/common/sim_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static sim_log_level g_level = SIM_LOG_INFO;
static FILE *g_file = NULL;
static bool g_use_stderr = true;

static const char *level_name(sim_log_level l) {
    switch (l) {
    case SIM_LOG_DEBUG: return "DEBUG";
    case SIM_LOG_INFO:  return "INFO";
    case SIM_LOG_WARN:  return "WARN";
    case SIM_LOG_ERROR: return "ERROR";
    default: return "?";
    }
}

void sim_log_init(sim_log_level level, const char *file) {
    g_level = level;
    if (file) {
        FILE *f = fopen(file, "a");
        if (f) { g_file = f; g_use_stderr = false; }
    }
}

void sim_log(sim_log_level level, const char *fmt, ...) {
    if ((int)level < (int)g_level) return;
    char ts[32];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    else ts[0] = '?', ts[1] = '\0';

    FILE *out = g_use_stderr ? stderr : g_file;
    fprintf(out, "[%s] [%s] ", ts, level_name(level));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}
