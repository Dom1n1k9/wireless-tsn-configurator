#ifndef WTSN_DB_DEVICES_H
#define WTSN_DB_DEVICES_H

#include "db/db.h"
#include "device/device.h"

wtsn_error wtsn_db_device_upsert(wtsn_db *db, const wtsn_device *dev);
wtsn_error wtsn_db_device_delete(wtsn_db *db, const char *id);
wtsn_error wtsn_db_device_get(wtsn_db *db, const char *id, wtsn_device *out);
typedef void (*wtsn_db_device_cb)(const wtsn_device *dev, void *userdata);
void wtsn_db_device_for_each(wtsn_db *db, wtsn_db_device_cb cb, void *userdata);
wtsn_error wtsn_db_device_set_status(wtsn_db *db, const char *id, wtsn_device_status status);
wtsn_error wtsn_db_device_touch(wtsn_db *db, const char *id, time_t seen);

#endif
