#ifndef WTSN_QOS_MANAGER_H
#define WTSN_QOS_MANAGER_H

#include "common/common.h"
#include "db/db.h"
#include "db/db_qos.h"
#include "mvc/event_bus.h"
#include "qos/qos.h"

#define WTSN_QOS_MANAGER_MODEL "qos"

typedef struct wtsn_qos_manager wtsn_qos_manager;

wtsn_qos_manager *wtsn_qos_manager_create(wtsn_db *db, wtsn_event_bus *bus);
void wtsn_qos_manager_destroy(wtsn_qos_manager *m);

wtsn_error wtsn_qos_manager_configure(wtsn_qos_manager *m, const char *device_id,
                                            const wtsn_qos_config_model *cfg);
wtsn_error wtsn_qos_manager_load_for_device(wtsn_qos_manager *m, const char *device_id, wtsn_qos_config *out);
wtsn_error wtsn_qos_manager_remove(wtsn_qos_manager *m, const char *device_id);

#endif
