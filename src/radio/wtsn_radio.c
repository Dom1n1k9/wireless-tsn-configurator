#include "radio/wtsn_radio.h"

#include <string.h>
#include <strings.h>

/* 802.1P priority -> WMM access category (802.11-2016 Table 9-2):
 *  7,6 -> VO (voice)
 *  5,4 -> VI (video)
 *  3,0 -> BE (best effort)
 *  2,1 -> BK (background)
 */
wtsn_wmm_ac wtsn_radio_map_priority(int priority) {
    switch (priority) {
    case 6:
    case 7:
        return WTSN_WMM_AC_VO;
    case 4:
    case 5:
        return WTSN_WMM_AC_VI;
    case 2:
    case 1:
        return WTSN_WMM_AC_BK;
    default:
        return WTSN_WMM_AC_BE;
    }
}

const char *wtsn_wmm_ac_str(wtsn_wmm_ac ac) {
    switch (ac) {
    case WTSN_WMM_AC_BK: return "AC_BK";
    case WTSN_WMM_AC_BE: return "AC_BE";
    case WTSN_WMM_AC_VI: return "AC_VI";
    case WTSN_WMM_AC_VO: return "AC_VO";
    default: return "AC_UNKNOWN";
    }
}

const char *wtsn_wmm_ac_description(wtsn_wmm_ac ac) {
    switch (ac) {
    case WTSN_WMM_AC_BK: return "background";
    case WTSN_WMM_AC_BE: return "best effort";
    case WTSN_WMM_AC_VI: return "video";
    case WTSN_WMM_AC_VO: return "voice (radio priority)";
    default: return "unknown";
    }
}

bool wtsn_radio_feature_supported(const char *feature) {
    if (!feature) return false;
    if (strstr(feature, "gPTP") || strstr(feature, "802.1AS")) return true;
    if (strstr(feature, "802.1Qbv")) return true;
    if (strstr(feature, "OPC UA") || strstr(feature, "FX")) return true;
    if (strstr(feature, "802.1Q") || strstr(feature, "VLAN")) return true;
    if (strstr(feature, "802.1Qav")) return true;
    /* 802.1Qbu preemption has no meaning inside a single radio link */
    if (strstr(feature, "802.1Qbu")) return false;
    return true;
}

const char *wtsn_radio_feature_hint(const char *feature) {
    if (!feature) return "";
    if (strstr(feature, "802.1Qbu"))
        return "preemption is wired-only; give the radio stream TSPEC admission control instead";
    return "";
}

int wtsn_radio_build_flow(int priority, int64_t interval_ns, int64_t burst_ns,
                          wtsn_radio_flow *out, size_t cap) {
    if (!out || cap == 0) return 0;
    size_t n = 0;
    wtsn_radio_flow f;
    f.priority = priority;
    f.ac = wtsn_radio_map_priority(priority);
    f.admitted = (f.ac == WTSN_WMM_AC_VO || f.ac == WTSN_WMM_AC_VI);
    f.interval_ns = interval_ns;
    f.burst_ns = burst_ns;
    out[n++] = f;
    return (int)n;
}
