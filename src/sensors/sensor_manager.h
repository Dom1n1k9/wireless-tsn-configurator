#ifndef WTSN_SENSOR_MANAGER_H
#define WTSN_SENSOR_MANAGER_H

#include "common/common.h"
#include "db/db.h"
#include "db/db_sensors.h"
#include "mvc/event_bus.h"
#include "sensors/sensor.h"

#define WTSN_SENSOR_MANAGER_MODEL "sensor"

typedef struct wtsn_sensor_manager wtsn_sensor_manager;

wtsn_sensor_manager *wtsn_sensor_manager_create(wtsn_db *db, wtsn_event_bus *bus);
void wtsn_sensor_manager_destroy(wtsn_sensor_manager *m);

wtsn_error wtsn_sensor_manager_upsert(wtsn_sensor_manager *m, const wtsn_sensor *s);
wtsn_error wtsn_sensor_manager_remove(wtsn_sensor_manager *m, const char *dev, const char *sid);
void wtsn_sensor_manager_for_each(wtsn_sensor_manager *m,
                                  void (*cb)(const wtsn_sensor *s, void *ud), void *ud);
size_t wtsn_sensor_manager_count(wtsn_sensor_manager *m);
wtsn_error wtsn_sensor_manager_auto_detect(wtsn_sensor_manager *m, const char *device_id);

#endif
