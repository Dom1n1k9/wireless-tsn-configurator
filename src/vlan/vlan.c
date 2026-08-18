#include "vlan/vlan.h"

#include <stdio.h>

wtsn_error wtsn_vlan_validate_group(const wtsn_vlan_group_model *g) {
    if (!g) return WTSN_ERR_INVALID_ARG;
    if (g->vlan_id < WTSN_VLAN_ID_MIN || g->vlan_id > WTSN_VLAN_ID_MAX)
        return WTSN_ERR_INVALID_ARG;
    if (strlen(g->name) == 0) return WTSN_ERR_INVALID_ARG;
    return WTSN_OK;
}

void wtsn_vlan_group_id(wtsn_vlan_group_model *g) {
    snprintf(g->id, sizeof(g->id), "vlan-%d", g->vlan_id);
}
