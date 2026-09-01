#include "db/db_config_versions.h"

#include "common/str_util.h"

#include <string.h>
#include <time.h>

wtsn_error wtsn_db_config_version_add(wtsn_db *db, const char *name,
                                     const char *device_id, const char *payload) {
    if (!db || !db->handle || !name) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "INSERT INTO config_versions (name,device_id,created_at,payload) VALUES (?,?,?,?);",
        -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, device_id ? device_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(st, 4, payload ? payload : "", -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}

wtsn_error wtsn_db_config_version_get(wtsn_db *db, int id, wtsn_config_version *out) {
    if (!db || !db->handle || !out) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "SELECT id,name,device_id,created_at,payload FROM config_versions WHERE id=?;",
        -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_int(st, 1, id);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return WTSN_ERR_NOT_FOUND; }
    memset(out, 0, sizeof(*out));
    out->id = sqlite3_column_int(st, 0);
    wtsn_strlcpy(out->name, (const char *)sqlite3_column_text(st, 1), sizeof(out->name));
    wtsn_strlcpy(out->device_id, (const char *)sqlite3_column_text(st, 2), sizeof(out->device_id));
    out->created_at = (time_t)sqlite3_column_int64(st, 3);
    wtsn_strlcpy(out->payload, (const char *)sqlite3_column_text(st, 4), sizeof(out->payload));
    sqlite3_finalize(st);
    return WTSN_OK;
}

void wtsn_db_config_version_for_each(wtsn_db *db, wtsn_db_config_version_cb cb, void *userdata) {
    if (!db || !db->handle || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "SELECT id,name,device_id,created_at,payload FROM config_versions ORDER BY id DESC;",
        -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        wtsn_config_version v;
        memset(&v, 0, sizeof(v));
        v.id = sqlite3_column_int(st, 0);
        wtsn_strlcpy(v.name, (const char *)sqlite3_column_text(st, 1), sizeof(v.name));
        wtsn_strlcpy(v.device_id, (const char *)sqlite3_column_text(st, 2), sizeof(v.device_id));
        v.created_at = (time_t)sqlite3_column_int64(st, 3);
        wtsn_strlcpy(v.payload, (const char *)sqlite3_column_text(st, 4), sizeof(v.payload));
        cb(&v, userdata);
    }
    sqlite3_finalize(st);
}

int wtsn_db_config_version_count(wtsn_db *db) {
    if (!db || !db->handle) return 0;
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(db->handle, "SELECT COUNT(*) FROM config_versions;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}
