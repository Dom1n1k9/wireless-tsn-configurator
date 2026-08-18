#ifndef WTSN_VLAN_H
#define WTSN_VLAN_H

#include "common/common.h"
#include "device/device.h"

#define WTSN_VLAN_ID_MIN 1
#define WTSN_VLAN_ID_MAX 4094

typedef struct {
    char id[WTSN_MAX_STR];
    char name[WTSN_MAX_STR];
    int vlan_id;
} wtsn_vlan_group_model;

typedef struct {
    char group_id[WTSN_MAX_STR];
    char device_id[WTSN_MAX_STR];
} wtsn_vlan_membership;

wtsn_error wtsn_vlan_validate_group(const wtsn_vlan_group_model *g);
void wtsn_vlan_group_id(wtsn_vlan_group_model *g);

#endif
