#ifndef WTSN_TIMESYNC_H
#define WTSN_TIMESYNC_H

#include "common/common.h"

#include <stdint.h>

typedef enum {
    WTSN_TIMESYNC_DISABLED = 0,
    WTSN_TIMESYNC_LOCAL_GRANDMASTER,
    WTSN_TIMESYNC_EXTERNAL_GRANDMASTER,
    WTSN_TIMESYNC_AUTO
} wtsn_timesync_mode;

typedef struct {
    wtsn_timesync_mode mode;
    char grandmaster[WTSN_MAX_STR];
    int64_t offset_ns;
    int quality;
    bool gptp_active;
    char protocol[32];
} wtsn_timesync_status;

const char *wtsn_timesync_mode_str(wtsn_timesync_mode m);
wtsn_timesync_mode wtsn_timesync_mode_parse(const char *s);
wtsn_error wtsn_timesync_validate_mode(wtsn_timesync_mode m);

#endif
