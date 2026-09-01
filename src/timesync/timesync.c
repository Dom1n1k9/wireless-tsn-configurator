#include "timesync/timesync.h"

#include <string.h>

const char *wtsn_timesync_mode_str(wtsn_timesync_mode m) {
    switch (m) {
    case WTSN_TIMESYNC_DISABLED: return "disabled";
    case WTSN_TIMESYNC_LOCAL_GRANDMASTER: return "local_grandmaster";
    case WTSN_TIMESYNC_EXTERNAL_GRANDMASTER: return "external_grandmaster";
    case WTSN_TIMESYNC_AUTO: return "auto";
    default: return "unknown";
    }
}

wtsn_timesync_mode wtsn_timesync_mode_parse(const char *s) {
    if (!s) return WTSN_TIMESYNC_DISABLED;
    if (strcmp(s, "local_grandmaster") == 0) return WTSN_TIMESYNC_LOCAL_GRANDMASTER;
    if (strcmp(s, "external_grandmaster") == 0) return WTSN_TIMESYNC_EXTERNAL_GRANDMASTER;
    if (strcmp(s, "auto") == 0) return WTSN_TIMESYNC_AUTO;
    return WTSN_TIMESYNC_DISABLED;
}

wtsn_error wtsn_timesync_validate_mode(wtsn_timesync_mode m) {
    return WTSN_OK;
}
