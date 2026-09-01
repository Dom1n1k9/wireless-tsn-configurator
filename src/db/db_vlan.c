#include "db/db_vlan.h"

#include "common/str_util.h"

#include <string.h>

static wtsn_vlan_group row_to_group(sqlite3_stmt *st) {
    wtsn_vlan_group g;
    memset(&g, 0, sizeof(g));
    wtsn_strlcpy(g.id, (const char *)sqlite3_column_text(st, 0), sizeof(g.id));
    wtsn_strlcpy(g.name, (const char *)sqlite3_column_text(st, 1), sizeof(g.name));
    g.vlan_id = sqlite3_column_int(st, 2);
    return g;
}

wtsn_error wtsn_db_vlan_group_save(wtsn_db *db, const wtsn_vlan_group *g) {
    if (!db || !db->handle || !g) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO vlan_groups (id,name,vlan_id) VALUES (?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET name=excluded.name, vlan_id=excluded.vlan_id;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, g->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, g->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, g->vlan_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? WTSN_OK : WTSN_ERR_DB;
}

wtsn_error wtsn_db_vlan_group_delete(wtsn_db *db, const char *id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "DELETE FROM vlan_groups WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
        return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}

void wtsn_db_vlan_group_for_each(wtsn_db *db, wtsn_db_vlan_group_cb cb, void *userdata) {
    if (!db || !db->handle || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "SELECT id,name,vlan_id FROM vlan_groups;", -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        wtsn_vlan_group g = row_to_group(st);
        if (cb(&g, userdata) != 0) break;
    }
    sqlite3_finalize(st);
}

wtsn_error wtsn_db_vlan_member_add(wtsn_db *db, const wtsn_vlan_member *m) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT OR IGNORE INTO vlan_members (group_id,device_id) VALUES (?,?);";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, m->group_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, m->device_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}

wtsn_error wtsn_db_vlan_member_remove(wtsn_db *db, const char *group_id, const char *device_id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "DELETE FROM vlan_members WHERE group_id=? AND device_id=?;", -1, &st, NULL) != SQLITE_OK)
        return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, group_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, device_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}

void wtsn_db_vlan_member_for_each_group(wtsn_db *db, const char *group_id,
                                        wtsn_db_vlan_member_cb cb, void *userdata) {
    if (!db || !db->handle || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "SELECT group_id,device_id FROM vlan_members WHERE group_id=?;", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, group_id, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        wtsn_vlan_member m;
        memset(&m, 0, sizeof(m));
        wtsn_strlcpy(m.group_id, (const char *)sqlite3_column_text(st, 0), sizeof(m.group_id));
        wtsn_strlcpy(m.device_id, (const char *)sqlite3_column_text(st, 1), sizeof(m.device_id));
        if (cb(&m, userdata) != 0) break;
    }
    sqlite3_finalize(st);
}

void wtsn_db_vlan_member_for_each_all(wtsn_db *db, wtsn_db_vlan_member_cb cb, void *userdata) {
    if (!db || !db->handle || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "SELECT group_id,device_id FROM vlan_members;", -1, &st, NULL) != SQLITE_OK)
        return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        wtsn_vlan_member m;
        memset(&m, 0, sizeof(m));
        wtsn_strlcpy(m.group_id, (const char *)sqlite3_column_text(st, 0), sizeof(m.group_id));
        wtsn_strlcpy(m.device_id, (const char *)sqlite3_column_text(st, 1), sizeof(m.device_id));
        if (cb(&m, userdata) != 0) break;
    }
    sqlite3_finalize(st);
}
