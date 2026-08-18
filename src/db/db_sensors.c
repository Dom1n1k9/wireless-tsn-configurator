#include "db/db_sensors.h"

#include <string.h>

wtsn_error wtsn_db_sensor_upsert(wtsn_db *db, const wtsn_sensor *s) {
    if (!db || !db->handle || !s) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO sensors (device_id,sensor_id,type,name,value,unit,healthy,last_update)"
        " VALUES (?,?,?,?,?,?,?,?)"
        " ON CONFLICT(device_id,sensor_id) DO UPDATE SET type=excluded.type,"
        " name=excluded.name, value=excluded.value, unit=excluded.unit,"
        " healthy=excluded.healthy, last_update=excluded.last_update;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, s->device_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, s->sensor_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, wtsn_sensor_type_str(s->type), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, s->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 5, s->value);
    sqlite3_bind_text(st, 6, s->unit, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 7, s->healthy ? 1 : 0);
    sqlite3_bind_int64(st, 8, (sqlite3_int64)s->last_update);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? WTSN_OK : WTSN_ERR_DB;
}

wtsn_error wtsn_db_sensor_delete(wtsn_db *db, const char *device_id, const char *sensor_id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "DELETE FROM sensors WHERE device_id=? AND sensor_id=?;", -1, &st, NULL) != SQLITE_OK)
        return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, device_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, sensor_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}

void wtsn_db_sensor_for_each(wtsn_db *db, wtsn_db_sensor_cb cb, void *userdata) {
    if (!db || !db->handle || !cb) return;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT device_id,sensor_id,type,name,value,unit,healthy,last_update FROM sensors;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        wtsn_sensor s;
        memset(&s, 0, sizeof(s));
        wtsn_strlcpy(s.device_id, (const char *)sqlite3_column_text(st, 0), sizeof(s.device_id));
        wtsn_strlcpy(s.sensor_id, (const char *)sqlite3_column_text(st, 1), sizeof(s.sensor_id));
        s.type = wtsn_sensor_type_parse((const char *)sqlite3_column_text(st, 2));
        wtsn_strlcpy(s.name, (const char *)sqlite3_column_text(st, 3), sizeof(s.name));
        s.value = sqlite3_column_double(st, 4);
        wtsn_strlcpy(s.unit, (const char *)sqlite3_column_text(st, 5), sizeof(s.unit));
        s.healthy = sqlite3_column_int(st, 6) != 0;
        s.last_update = (time_t)sqlite3_column_int64(st, 7);
        cb(&s, userdata);
    }
    sqlite3_finalize(st);
}
