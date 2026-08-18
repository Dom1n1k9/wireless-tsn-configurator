#ifndef WTSN_DB_VLAN_H
#define WTSN_DB_VLAN_H

#include "db/db.h"
#include "device/device.h"

typedef struct {
    char id[WTSN_MAX_STR];
    char name[WTSN_MAX_STR];
    int vlan_id;
} wtsn_vlan_group;

typedef struct {
    char group_id[WTSN_MAX_STR];
    char device_id[WTSN_MAX_STR];
} wtsn_vlan_member;

wtsn_error wtsn_db_vlan_group_save(wtsn_db *db, const wtsn_vlan_group *g);
wtsn_error wtsn_db_vlan_group_delete(wtsn_db *db, const char *id);
typedef int (*wtsn_db_vlan_group_cb)(const wtsn_vlan_group *g, void *userdata);
void wtsn_db_vlan_group_for_each(wtsn_db *db, wtsn_db_vlan_group_cb cb, void *userdata);

wtsn_error wtsn_db_vlan_member_add(wtsn_db *db, const wtsn_vlan_member *m);
wtsn_error wtsn_db_vlan_member_remove(wtsn_db *db, const char *group_id, const char *device_id);
typedef int (*wtsn_db_vlan_member_cb)(const wtsn_vlan_member *m, void *userdata);
void wtsn_db_vlan_member_for_each_group(wtsn_db *db, const char *group_id, wtsn_db_vlan_member_cb cb, void *userdata);

#endif
