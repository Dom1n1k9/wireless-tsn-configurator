#include "stream/stream.h"

#include <string.h>

wtsn_error wtsn_stream_validate(const wtsn_stream *s) {
    if (!s) return WTSN_ERR_INVALID_ARG;
    if (strlen(s->stream_id) == 0) return WTSN_ERR_INVALID_ARG;
    if (strlen(s->talker) == 0) return WTSN_ERR_INVALID_ARG;
    if (s->vlan_id < 0 || s->vlan_id > 4094) return WTSN_ERR_INVALID_ARG;
    if (s->priority < 0 || s->priority > 7) return WTSN_ERR_INVALID_ARG;
    if (s->data_frame_prio < 0 || s->data_frame_prio > 7)
        return WTSN_ERR_INVALID_ARG;
    if (s->max_latency_ns <= 0) return WTSN_ERR_INVALID_ARG;
    if (s->max_interval_ns <= 0) return WTSN_ERR_INVALID_ARG;
    /* every stream needs at least one listener */
    if (!s->listener_all && s->listener_count == 0) return WTSN_ERR_INVALID_ARG;
    return WTSN_OK;
}

wtsn_stream_status wtsn_stream_status_parse(const char *s) {
    if (!s) return WTSN_STREAM_CONFIGURED;
    if (strcmp(s, "ready") == 0) return WTSN_STREAM_READY;
    if (strcmp(s, "failed") == 0) return WTSN_STREAM_FAILED;
    if (strcmp(s, "standby") == 0) return WTSN_STREAM_STANDBY;
    return WTSN_STREAM_CONFIGURED;
}

const char *wtsn_stream_status_str(wtsn_stream_status st) {
    switch (st) {
    case WTSN_STREAM_READY: return "ready";
    case WTSN_STREAM_FAILED: return "failed";
    case WTSN_STREAM_STANDBY: return "standby";
    default: return "configured";
    }
}

const char *wtsn_stream_role_str(wtsn_stream_role r) {
    switch (r) {
    case WTSN_STREAM_ROLE_TALKER: return "talker";
    default: return "listener";
    }
}
