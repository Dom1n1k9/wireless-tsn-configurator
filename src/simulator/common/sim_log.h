#ifndef SIM_LOG_H
#define SIM_LOG_H

typedef enum {
    SIM_LOG_DEBUG = 0,
    SIM_LOG_INFO,
    SIM_LOG_WARN,
    SIM_LOG_ERROR
} sim_log_level;

void sim_log_init(sim_log_level level, const char *file);
void sim_log(sim_log_level level, const char *fmt, ...);

#endif
