#ifndef WTSN_DISCOVERY_H
#define WTSN_DISCOVERY_H

#include "common/common.h"
#include "device/device.h"

typedef enum {
    WTSN_DISCOVERY_MQTT = 0,
    WTSN_DISCOVERY_PLUGIN,
    WTSN_DISCOVERY_MANUAL
} wtsn_discovery_source;

typedef struct wtsn_discoverer wtsn_discoverer;

typedef wtsn_error (*wtsn_discovery_run_fn)(wtsn_discoverer *d, wtsn_device *out, int max, int *count);
typedef void (*wtsn_discovery_destroy_fn)(wtsn_discoverer *d);

struct wtsn_discoverer {
    wtsn_discovery_source source;
    char name[WTSN_MAX_STR];
    void *data;
    wtsn_discovery_run_fn run;
    wtsn_discovery_destroy_fn destroy;
    void (*on_device)(wtsn_device *dev, void *ud);
    void *userdata;
};

typedef wtsn_discoverer *(*wtsn_discovery_create_fn)(wtsn_discovery_source src);

wtsn_discoverer *wtsn_discovery_create(wtsn_discovery_source src, const char *name,
                                       wtsn_discovery_run_fn run,
                                       wtsn_discovery_destroy_fn destroy, void *data);
void wtsn_discovery_destroy(wtsn_discoverer *d);

#endif
