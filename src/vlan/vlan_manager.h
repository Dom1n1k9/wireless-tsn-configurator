#ifndef WTSN_VLAN_MANAGER_H
#define WTSN_VLAN_MANAGER_H

#include "common/common.h"
#include "db/db.h"
#include "db/db_vlan.h"
#include "mvc/event_bus.h"
#include "vlan/vlan.h"

#define WTSN_VLAN_MANAGER_MODEL "vlan"

typedef struct wtsn_vlan_manager wtsn_vlan_manager;

wtsn_vlan_manager *wtsn_vlan_manager_create(wtsn_db *db, wtsn_event_bus *bus);
void wtsn_vlan_manager_destroy(wtsn_vlan_manager *m);

wtsn_error wtsn_vlan_manager_create_group(wtsn_vlan_manager *m, const wtsn_vlan_group_model *g);
wtsn_error wtsn_vlan_manager_delete_group(wtsn_vlan_manager *m, const char *id);
wtsn_error wtsn_vlan_manager_add_member(wtsn_vlan_manager *m, const char *group_id, const char *device_id);
wtsn_error wtsn_vlan_manager_remove_member(wtsn_vlan_manager *m, const char *group_id, const char *device_id);

wtsn_error wtsn_vlan_manager_import(wtsn_vlan_manager *m, const char *file);
wtsn_error wtsn_vlan_manager_export(wtsn_vlan_manager *m, const char *file);

#endif
