#include "db/db_qos.h"

#include <string.h>

wtsn_error wtsn_db_qos_save(wtsn_db *db, const wtsn_qos_config *cfg) {
    if (!db || !db->handle || !cfg) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO qos_configs (device_id,priority,traffic_class,bandwidth_kbps,latency_ms)"
        " VALUES (?,?,?,?,?)"
        " ON CONFLICT(device_id) DO UPDATE SET priority=excluded.priority,"
        " traffic_class=excluded.traffic_class, bandwidth_kbps=excluded.bandwidth_kbps,"
        " latency_ms=excluded.latency_ms;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, cfg->device_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, cfg->priority);
    sqlite3_bind_int(st, 3, cfg->traffic_class);
    sqlite3_bind_int(st, 4, cfg->bandwidth_kbps);
    sqlite3_bind_int(st, 5, cfg->latency_ms);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? WTSN_OK : WTSN_ERR_DB;
}

wtsn_error wtsn_db_qos_load(wtsn_db *db, const char *device_id, wtsn_qos_config *out) {
    if (!db || !db->handle || !device_id || !out) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT device_id,priority,traffic_class,bandwidth_kbps,latency_ms"
        " FROM qos_configs WHERE device_id=?;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, device_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return WTSN_ERR_NOT_FOUND; }
    memset(out, 0, sizeof(*out));
    wtsn_strlcpy(out->device_id, (const char *)sqlite3_column_text(st, 0), sizeof(out->device_id));
    out->priority = sqlite3_column_int(st, 1);
    out->traffic_class = sqlite3_column_int(st, 2);
    out->bandwidth_kbps = sqlite3_column_int(st, 3);
    out->latency_ms = sqlite3_column_int(st, 4);
    sqlite3_finalize(st);
    return WTSN_OK;
}

wtsn_error wtsn_db_qos_delete(wtsn_db *db, const char *device_id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "DELETE FROM qos_configs WHERE device_id=?;", -1, &st, NULL) != SQLITE_OK)
        return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, device_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}
