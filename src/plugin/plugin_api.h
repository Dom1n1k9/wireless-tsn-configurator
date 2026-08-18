#ifndef WTSN_PLUGIN_API_H
#define WTSN_PLUGIN_API_H

#include "common/common.h"
#include "device/device.h"

#define WTSN_PLUGIN_API_VERSION 1

typedef struct wtsn_plugin wtsn_plugin;

struct wtsn_plugin {
    int api_version;
    char name[WTSN_MAX_STR];
    void *handle;
    void *userdata;
    wtsn_error (*probe)(wtsn_plugin *self, const char *discovery_data);
    wtsn_error (*discover)(wtsn_plugin *self, wtsn_device **out, int max, int *count);
    wtsn_error (*shutdown)(wtsn_plugin *self);
};

typedef wtsn_plugin *(*wtsn_plugin_create_fn)(void);
typedef void (*wtsn_plugin_destroy_fn)(wtsn_plugin *);

#endif
