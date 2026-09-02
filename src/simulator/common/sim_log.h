#ifndef SIM_LOG_H
#define SIM_LOG_H

/* The simulator reuses the core log implementation (src/common/log.c).
 * sim_* names are kept as aliases so simulator sources stay unchanged. */
#include "common/log.h"

#define SIM_LOG_DEBUG WTSN_LOG_DEBUG
#define SIM_LOG_INFO  WTSN_LOG_INFO
#define SIM_LOG_WARN  WTSN_LOG_WARN
#define SIM_LOG_ERROR WTSN_LOG_ERROR

typedef wtsn_log_level sim_log_level;
#define sim_log        wtsn_log
#define sim_log_init   wtsn_log_init

#endif
