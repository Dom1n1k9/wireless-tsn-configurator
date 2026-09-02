#include "device/device.h"

#include "common/str_util.h"

#include <string.h>
#include <strings.h>

const char *wtsn_device_status_str(wtsn_device_status s) {
    switch (s) {
    case WTSN_DEVICE_ONLINE: return "online";
    case WTSN_DEVICE_OFFLINE: return "offline";
    case WTSN_DEVICE_ERROR: return "error";
    default: return "unknown";
    }
}

const char *wtsn_device_kind_str(wtsn_device_kind k) {
    switch (k) {
    case WTSN_DEVICE_KIND_ESP32: return "ESP32";
    case WTSN_DEVICE_KIND_RASPBERRYPI: return "RaspberryPi";
    case WTSN_DEVICE_KIND_STM32: return "STM32";
    default: return "Unknown";
    }
}

wtsn_device_kind wtsn_device_kind_parse(const char *s) {
    if (!s) return WTSN_DEVICE_KIND_UNKNOWN;
    if (strcasecmp(s, "ESP32") == 0) return WTSN_DEVICE_KIND_ESP32;
    if (strcasecmp(s, "RaspberryPi") == 0) return WTSN_DEVICE_KIND_RASPBERRYPI;
    if (strcasecmp(s, "STM32") == 0) return WTSN_DEVICE_KIND_STM32;
    return WTSN_DEVICE_KIND_UNKNOWN;
}

void wtsn_device_add_tsn_feature(wtsn_device *d, const char *feature) {
    if (!d || !feature || d->tsn_features_count >= WTSN_TSN_FEATURES_MAX) return;
    wtsn_strlcpy(d->tsn_features[d->tsn_features_count], feature, 64);
    d->tsn_features_count++;
}
