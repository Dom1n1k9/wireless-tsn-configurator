#ifndef WTSN_PLUGIN_MANAGER_H
#define WTSN_PLUGIN_MANAGER_H

#include "common/common.h"
#include "plugin/plugin_api.h"

typedef struct wtsn_plugin_manager wtsn_plugin_manager;

wtsn_plugin_manager *wtsn_plugin_manager_create(void);
void wtsn_plugin_manager_destroy(wtsn_plugin_manager *m);
wtsn_error wtsn_plugin_manager_load(wtsn_plugin_manager *m, const char *path);
size_t wtsn_plugin_manager_count(wtsn_plugin_manager *m);
wtsn_plugin *wtsn_plugin_manager_get(wtsn_plugin_manager *m, size_t index);

#endif
