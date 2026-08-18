#include "vlan/vlan_manager.h"

#include "common/log.h"
#include "mvc/model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wtsn_vlan_manager {
    wtsn_db *db;
    wtsn_event_bus *bus;
    wtsn_model model;
};

wtsn_vlan_manager *wtsn_vlan_manager_create(wtsn_db *db, wtsn_event_bus *bus) {
    if (!db || !bus) return NULL;
    wtsn_vlan_manager *m = calloc(1, sizeof(wtsn_vlan_manager));
    if (!m) return NULL;
    m->db = db;
    m->bus = bus;
    wtsn_model_init(&m->model, WTSN_VLAN_MANAGER_MODEL, bus);
    return m;
}

void wtsn_vlan_manager_destroy(wtsn_vlan_manager *m) {
    free(m);
}

wtsn_error wtsn_vlan_manager_create_group(wtsn_vlan_manager *m, const wtsn_vlan_group_model *g) {
    if (!m || !g) return WTSN_ERR_INVALID_ARG;
    wtsn_error e = wtsn_vlan_validate_group(g);
    if (e != WTSN_OK) return e;
    wtsn_vlan_group tmp;
    memset(&tmp, 0, sizeof(tmp));
    wtsn_strlcpy(tmp.id, g->id, sizeof(tmp.id));
    wtsn_strlcpy(tmp.name, g->name, sizeof(tmp.name));
    tmp.vlan_id = g->vlan_id;
    wtsn_db_vlan_group_save(m->db, &tmp);
    wtsn_model_notify(&m->model, "group_changed");
    return WTSN_OK;
}

wtsn_error wtsn_vlan_manager_delete_group(wtsn_vlan_manager *m, const char *id) {
    if (!m || !id) return WTSN_ERR_INVALID_ARG;
    wtsn_db_vlan_group_delete(m->db, id);
    wtsn_model_notify(&m->model, "group_changed");
    return WTSN_OK;
}

wtsn_error wtsn_vlan_manager_add_member(wtsn_vlan_manager *m, const char *group_id, const char *device_id) {
    if (!m || !group_id || !device_id) return WTSN_ERR_INVALID_ARG;
    wtsn_vlan_member mem;
    memset(&mem, 0, sizeof(mem));
    wtsn_strlcpy(mem.group_id, group_id, sizeof(mem.group_id));
    wtsn_strlcpy(mem.device_id, device_id, sizeof(mem.device_id));
    wtsn_db_vlan_member_add(m->db, &mem);
    wtsn_model_notify(&m->model, "member_changed");
    return WTSN_OK;
}

wtsn_error wtsn_vlan_manager_remove_member(wtsn_vlan_manager *m, const char *group_id, const char *device_id) {
    if (!m || !group_id || !device_id) return WTSN_ERR_INVALID_ARG;
    wtsn_db_vlan_member_remove(m->db, group_id, device_id);
    wtsn_model_notify(&m->model, "member_changed");
    return WTSN_OK;
}

struct import_ctx {
    wtsn_vlan_manager *m;
};

static int export_group_cb(const wtsn_vlan_group *g, void *ud) {
    FILE *f = (FILE *)ud;
    fprintf(f, "VLAN,%s,%d\n", g->name, g->vlan_id);
    return 0;
}

wtsn_error wtsn_vlan_manager_import(wtsn_vlan_manager *m, const char *file) {
    if (!m || !file) return WTSN_ERR_INVALID_ARG;
    FILE *f = fopen(file, "r");
    if (!f) return WTSN_ERR_IO;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char name[WTSN_MAX_STR] = "";
        int vlan_id = 0;
        if (sscanf(line, "VLAN,%255[^,],%d", name, &vlan_id) == 2) {
            wtsn_vlan_group_model g;
            memset(&g, 0, sizeof(g));
            wtsn_strlcpy(g.name, name, sizeof(g.name));
            g.vlan_id = vlan_id;
            wtsn_vlan_group_id(&g);
            wtsn_vlan_manager_create_group(m, &g);
        }
    }
    fclose(f);
    return WTSN_OK;
}

wtsn_error wtsn_vlan_manager_export(wtsn_vlan_manager *m, const char *file) {
    if (!m || !file) return WTSN_ERR_INVALID_ARG;
    FILE *f = fopen(file, "w");
    if (!f) return WTSN_ERR_IO;
    wtsn_db_vlan_group_for_each(m->db, export_group_cb, f);
    fclose(f);
    return WTSN_OK;
}
