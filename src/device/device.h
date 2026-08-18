#ifndef WTSN_DEVICE_H
#define WTSN_DEVICE_H

#include "common/common.h"

#include <time.h>

#define WTSN_TSN_FEATURES_MAX 8

typedef enum {
    WTSN_DEVICE_ONLINE = 0,
    WTSN_DEVICE_OFFLINE,
    WTSN_DEVICE_ERROR
} wtsn_device_status;

typedef enum {
    WTSN_DEVICE_KIND_UNKNOWN = 0,
    WTSN_DEVICE_KIND_ESP32,
    WTSN_DEVICE_KIND_RASPBERRYPI,
    WTSN_DEVICE_KIND_STM32
} wtsn_device_kind;

typedef struct {
    char id[WTSN_MAX_STR];
    char ip[64];
    char mac[32];
    char firmware[32];
    time_t last_seen;
    wtsn_device_status status;
    wtsn_device_kind kind;
    char tsn_features[WTSN_TSN_FEATURES_MAX][64];
    size_t tsn_features_count;
    char name[WTSN_MAX_STR];
} wtsn_device;

const char *wtsn_device_status_str(wtsn_device_status s);
const char *wtsn_device_kind_str(wtsn_device_kind k);
wtsn_device_kind wtsn_device_kind_parse(const char *s);
void wtsn_device_add_tsn_feature(wtsn_device *d, const char *feature);

#endif
