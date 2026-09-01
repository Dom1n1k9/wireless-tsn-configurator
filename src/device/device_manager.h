#ifndef WTSN_DEVICE_MANAGER_H
#define WTSN_DEVICE_MANAGER_H

#include "common/common.h"
#include "db/db.h"
#include "device/device.h"
#include "mvc/event_bus.h"
#include "plugin/plugin_manager.h"

typedef struct wtsn_device_manager wtsn_device_manager;

wtsn_device_manager *wtsn_device_manager_create(wtsn_db *db, wtsn_event_bus *bus,
                                                 wtsn_plugin_manager *plugins);
void wtsn_device_manager_destroy(wtsn_device_manager *m);

wtsn_error wtsn_device_manager_upsert(wtsn_device_manager *m, const wtsn_device *dev);
wtsn_error wtsn_device_manager_remove(wtsn_device_manager *m, const char *id);
const wtsn_device *wtsn_device_manager_get(wtsn_device_manager *m, const char *id);
size_t wtsn_device_manager_count(wtsn_device_manager *m);
void wtsn_device_manager_for_each(wtsn_device_manager *m,
                                  void (*cb)(const wtsn_device *dev, void *ud), void *ud);

void wtsn_device_manager_mark_offline_after(wtsn_device_manager *m, time_t threshold);
void wtsn_device_manager_restore(wtsn_device_manager *m);
wtsn_error wtsn_device_manager_discover_once(wtsn_device_manager *m);
wtsn_error wtsn_device_manager_record_heartbeat(wtsn_device_manager *m, const char *id);
wtsn_error wtsn_device_manager_set_domain(wtsn_device_manager *m, const char *id,
                                        const char *domain);
wtsn_error wtsn_device_manager_set_health(wtsn_device_manager *m, const char *id,
                                        wtsn_device_status status);

#endif
