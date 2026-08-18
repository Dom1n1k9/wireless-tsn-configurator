#ifndef WTSN_QOS_H
#define WTSN_QOS_H

#include "common/common.h"

typedef enum {
    WTSN_QOS_TC_BEST_EFFORT = 0,
    WTSN_QOS_TC_AUDIO_VIDEO,
    WTSN_QOS_TC_CONTROLLED,
    WTSN_QOS_TC_CRITICAL
} wtsn_qos_traffic_class;

typedef enum {
    WTSN_QOS_LATENCY_PRIORITY = 0,
    WTSN_QOS_LATENCY_SOFT_REAL_TIME,
    WTSN_QOS_LATENCY_HARD_REAL_TIME
} wtsn_qos_latency_class;

typedef struct {
    char device_id[WTSN_MAX_STR];
    int priority;
    wtsn_qos_traffic_class traffic_class;
    int bandwidth_kbps;
    int latency_ms;
    wtsn_qos_latency_class latency_class;
} wtsn_qos_config_model;

wtsn_error wtsn_qos_validate(const wtsn_qos_config_model *cfg);
const char *wtsn_qos_tc_str(wtsn_qos_traffic_class tc);
const char *wtsn_qos_latency_str(wtsn_qos_latency_class lc);

#endif
