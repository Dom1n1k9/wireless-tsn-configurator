#include "db/db_devices.h"

#include <string.h>

static void bind_device(sqlite3_stmt *st, const wtsn_device *dev) {
    sqlite3_bind_text(st, 1, dev->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, dev->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, dev->ip, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, dev->mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, wtsn_device_kind_str(dev->kind), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, dev->firmware, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 7, (int)dev->status);
    sqlite3_bind_int64(st, 8, (long long)dev->last_seen);
    sqlite3_bind_text(st, 9, dev->domain[0] ? dev->domain : "default", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 10, (long long)dev->heartbeat_at);
}

wtsn_error wtsn_db_device_upsert(wtsn_db *db, const wtsn_device *dev) {
    if (!db || !dev || !db->handle) return WTSN_ERR_INVALID_ARG;
    const char *sql =
        "INSERT INTO devices (id,name,ip,mac,kind,firmware,status,last_seen,domain,heartbeat_at)"
        " VALUES (?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET"
        " name=excluded.name, ip=excluded.ip, mac=excluded.mac,"
        " kind=excluded.kind, firmware=excluded.firmware, status=excluded.status,"
        " last_seen=excluded.last_seen, domain=excluded.domain,"
        " heartbeat_at=excluded.heartbeat_at;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK)
        return WTSN_ERR_DB;
    bind_device(st, dev);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return WTSN_ERR_DB;

    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "DELETE FROM device_tsn_features WHERE device_id=?;", -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_text(del, 1, dev->id, -1, SQLITE_TRANSIENT);
        sqlite3_step(del);
        sqlite3_finalize(del);
    }
    for (size_t i = 0; i < dev->tsn_features_count; i++) {
        sqlite3_stmt *f = NULL;
        if (sqlite3_prepare_v2(db->handle,
            "INSERT INTO device_tsn_features (device_id,feature) VALUES (?,?);",
            -1, &f, NULL) != SQLITE_OK) continue;
        sqlite3_bind_text(f, 1, dev->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(f, 2, dev->tsn_features[i], -1, SQLITE_TRANSIENT);
        sqlite3_step(f);
        sqlite3_finalize(f);
    }
    return WTSN_OK;
}

static wtsn_device row_to_device(const wtsn_db *db, sqlite3_stmt *st) {
    wtsn_device d;
    memset(&d, 0, sizeof(d));
    wtsn_strlcpy(d.id, (const char *)sqlite3_column_text(st, 0), sizeof(d.id));
    wtsn_strlcpy(d.name, (const char *)sqlite3_column_text(st, 1), sizeof(d.name));
    wtsn_strlcpy(d.ip, (const char *)sqlite3_column_text(st, 2), sizeof(d.ip));
    wtsn_strlcpy(d.mac, (const char *)sqlite3_column_text(st, 3), sizeof(d.mac));
    d.kind = wtsn_device_kind_parse((const char *)sqlite3_column_text(st, 4));
    wtsn_strlcpy(d.firmware, (const char *)sqlite3_column_text(st, 5), sizeof(d.firmware));
    d.status = (wtsn_device_status)sqlite3_column_int(st, 6);
    d.last_seen = (time_t)sqlite3_column_int64(st, 7);
    const char *dom = (const char *)sqlite3_column_text(st, 8);
    if (dom && dom[0]) wtsn_strlcpy(d.domain, dom, sizeof(d.domain));
    d.heartbeat_at = (time_t)sqlite3_column_int64(st, 9);

    sqlite3_stmt *f = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "SELECT feature FROM device_tsn_features WHERE device_id=?;", -1, &f, NULL) == SQLITE_OK) {
        sqlite3_bind_text(f, 1, d.id, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(f) == SQLITE_ROW) {
            wtsn_device_add_tsn_feature(&d, (const char *)sqlite3_column_text(f, 0));
        }
        sqlite3_finalize(f);
    }
    return d;
}

void wtsn_db_device_for_each(wtsn_db *db, wtsn_db_device_cb cb, void *userdata) {
    if (!db || !db->handle || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "SELECT id,name,ip,mac,kind,firmware,status,last_seen,domain,heartbeat_at FROM devices;",
        -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        wtsn_device d = row_to_device(db, st);
        cb(&d, userdata);
    }
    sqlite3_finalize(st);
}

wtsn_error wtsn_db_device_get(wtsn_db *db, const char *id, wtsn_device *out) {
    if (!db || !db->handle || !id || !out) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "SELECT id,name,ip,mac,kind,firmware,status,last_seen,domain,heartbeat_at FROM devices WHERE id=?;",
        -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    bool found = (rc == SQLITE_ROW);
    if (found) *out = row_to_device(db, st);
    sqlite3_finalize(st);
    return found ? WTSN_OK : WTSN_ERR_NOT_FOUND;
}

wtsn_error wtsn_db_device_delete(wtsn_db *db, const char *id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "DELETE FROM devices WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
        return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}

wtsn_error wtsn_db_device_set_status(wtsn_db *db, const char *id, wtsn_device_status status) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "UPDATE devices SET status=? WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
        return WTSN_ERR_DB;
    sqlite3_bind_int(st, 1, (int)status);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}

wtsn_error wtsn_db_device_touch(wtsn_db *db, const char *id, time_t seen) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "UPDATE devices SET last_seen=? WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
        return WTSN_ERR_DB;
    sqlite3_bind_int64(st, 1, (long long)seen);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}
