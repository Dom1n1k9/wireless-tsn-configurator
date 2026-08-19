#include "qos/qos.h"

#include "common/log.h"

wtsn_error wtsn_qos_validate(const wtsn_qos_config_model *cfg) {
    if (!cfg) return WTSN_ERR_INVALID_ARG;
    if (cfg->priority < 0 || cfg->priority > 7)
        return WTSN_ERR_INVALID_ARG;
    if (cfg->bandwidth_kbps < 0 || cfg->bandwidth_kbps > 10000000)
        return WTSN_ERR_INVALID_ARG;
    if (cfg->latency_ms < 0 || cfg->latency_ms > 60000)
        return WTSN_ERR_INVALID_ARG;
    if (strlen(cfg->device_id) == 0)
        return WTSN_ERR_INVALID_ARG;
    return WTSN_OK;
}

const char *wtsn_qos_tc_str(wtsn_qos_traffic_class tc) {
    switch (tc) {
    case WTSN_QOS_TC_BEST_EFFORT: return "Best Effort";
    case WTSN_QOS_TC_AUDIO_VIDEO: return "Audio/Video";
    case WTSN_QOS_TC_CONTROLLED: return "Controlled";
    case WTSN_QOS_TC_CRITICAL: return "Critical";
    default: return "Unknown";
    }
}

const char *wtsn_qos_latency_str(wtsn_qos_latency_class lc) {
    switch (lc) {
    case WTSN_QOS_LATENCY_PRIORITY: return "Priority";
    case WTSN_QOS_LATENCY_SOFT_REAL_TIME: return "Soft Real-Time";
    case WTSN_QOS_LATENCY_HARD_REAL_TIME: return "Hard Real-Time";
    default: return "Unknown";
    }
}

const char *wtsn_preemption_str(wtsn_frame_preemption p) {
    switch (p) {
    case WTSN_PREEMPT_OFF: return "off";
    case WTSN_PREEMPT_EXPRESS_QUEUE: return "express-queue";
    case WTSN_PREEMPT_ON: return "on";
    default: return "off";
    }
}
