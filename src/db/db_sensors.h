#ifndef WTSN_DB_SENSORS_H
#define WTSN_DB_SENSORS_H

#include "db/db.h"
#include "sensors/sensor.h"

wtsn_error wtsn_db_sensor_upsert(wtsn_db *db, const wtsn_sensor *s);
wtsn_error wtsn_db_sensor_delete(wtsn_db *db, const char *device_id, const char *sensor_id);
typedef void (*wtsn_db_sensor_cb)(const wtsn_sensor *s, void *userdata);
void wtsn_db_sensor_for_each(wtsn_db *db, wtsn_db_sensor_cb cb, void *userdata);

#endif
