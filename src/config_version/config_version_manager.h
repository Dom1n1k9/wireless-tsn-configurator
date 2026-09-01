#ifndef WTSN_CONFIG_VERSION_MANAGER_H
#define WTSN_CONFIG_VERSION_MANAGER_H

#include "common/common.h"
#include "db/db_config_versions.h"
#include "mvc/event_bus.h"

#define WTSN_CONFIG_VERSION_MODEL "config_version"

typedef struct wtsn_config_version_manager wtsn_config_version_manager;

wtsn_config_version_manager *wtsn_cfg_ver_manager_create(wtsn_db *db, wtsn_event_bus *bus);
void wtsn_cfg_ver_manager_destroy(wtsn_config_version_manager *m);

/* snapshot current config for a device (or global) and store it as a version */
wtsn_error wtsn_cfg_ver_snapshot(wtsn_config_version_manager *m, const char *name,
                                 const char *device_id);
wtsn_error wtsn_cfg_ver_rollback(wtsn_config_version_manager *m, int id);
wtsn_error wtsn_cfg_ver_diff(wtsn_config_version_manager *m, int id_a, int id_b,
                             char *out, size_t out_size);
int wtsn_cfg_ver_count(wtsn_config_version_manager *m);
void wtsn_cfg_ver_for_each(wtsn_config_version_manager *m, wtsn_db_config_version_cb cb,
                           void *ud);

#endif
