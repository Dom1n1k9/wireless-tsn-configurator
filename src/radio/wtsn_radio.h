#ifndef WTSN_RADIO_H
#define WTSN_RADIO_H

#include "common/common.h"

/* Wi-Fi Multi-Media (WMM / 802.11e) access categories that map 802.1P
 * priorities to the radio queue that carries them. This is the wireless
 * analog of wired egress queueing.
 */
typedef enum {
    WTSN_WMM_AC_BK = 0,   /* background */
    WTSN_WMM_AC_BE,       /* best effort */
    WTSN_WMM_AC_VI,       /* video */
    WTSN_WMM_AC_VO,       /* voice */
    WTSN_WMM_AC_COUNT
} wtsn_wmm_ac;

typedef struct {
    int priority;          /* 802.1P / PCP 0-7 */
    wtsn_wmm_ac ac;        /* mapped WMM access category */
    bool admitted;         /* TSPEC admission-controlled stream */
    int64_t interval_ns;   /* requested service interval for stream */
    int64_t burst_ns;      /* nominal burst size */
} wtsn_radio_flow;

wtsn_wmm_ac wtsn_radio_map_priority(int priority);
const char *wtsn_wmm_ac_str(wtsn_wmm_ac ac);
const char *wtsn_wmm_ac_description(wtsn_wmm_ac ac);

/* Check whether a wired TSN feature is realisable over 802.11 and
 * return a human-readable hint if not. */
const char *wtsn_radio_feature_hint(const char *feature);
bool wtsn_radio_feature_supported(const char *feature);

/* Build a radio flow description for a stream on a given radio. */
int wtsn_radio_build_flow(int priority, int64_t interval_ns, int64_t burst_ns,
                          wtsn_radio_flow *out, size_t cap);

#endif
