#include "db/db.h"

#include "common/log.h"

#include <string.h>

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS devices ("
    "  id TEXT PRIMARY KEY, name TEXT, ip TEXT, mac TEXT, kind TEXT,"
    "  firmware TEXT, status INTEGER, last_seen INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS device_tsn_features ("
    "  device_id TEXT, feature TEXT,"
    "  FOREIGN KEY(device_id) REFERENCES devices(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS qos_configs ("
    "  device_id TEXT PRIMARY KEY, priority INTEGER, traffic_class INTEGER,"
    "  bandwidth_kbps INTEGER, latency_ms INTEGER, preemption INTEGER,"
    "  FOREIGN KEY(device_id) REFERENCES devices(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS vlan_groups ("
    "  id TEXT PRIMARY KEY, name TEXT, vlan_id INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS vlan_members ("
    "  group_id TEXT, device_id TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS tas_schedules ("
    "  id TEXT PRIMARY KEY, name TEXT, cycle_time_ns INTEGER, deploy_target TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS gcl_entries ("
    "  schedule_id TEXT, index INTEGER, gate_state TEXT, duration_ns INTEGER,"
    "  FOREIGN KEY(schedule_id) REFERENCES tas_schedules(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS timesync_status ("
    "  id TEXT PRIMARY KEY, mode TEXT, grandmaster TEXT, offset_ns INTEGER,"
    "  quality INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS sensors ("
    "  device_id TEXT, sensor_id TEXT, type TEXT, name TEXT, value REAL,"
    "  unit TEXT, healthy INTEGER, last_update INTEGER,"
    "  PRIMARY KEY(device_id, sensor_id),"
    "  FOREIGN KEY(device_id) REFERENCES devices(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS settings ("
    "  key TEXT PRIMARY KEY, value TEXT"
    ");";

static wtsn_error run_migration(wtsn_db *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db->handle, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK && err != NULL) {
        if (strstr(err, "duplicate column name") == NULL) {
            wtsn_log(WTSN_LOG_WARN, "migration step skipped: %s", err);
        }
        sqlite3_free(err);
    }
    return WTSN_OK;
}

wtsn_error wtsn_db_open(wtsn_db *db, const char *path) {
    if (!db || !path) return WTSN_ERR_INVALID_ARG;
    memset(db, 0, sizeof(*db));
    int rc = sqlite3_open(path, &db->handle);
    if (rc != SQLITE_OK) {
        wtsn_log(WTSN_LOG_ERROR, "sqlite open failed: %s", sqlite3_errmsg(db->handle));
        return WTSN_ERR_DB;
    }
    wtsn_strlcpy(db->path, path, sizeof(db->path));
    return wtsn_db_migrate(db);
}

wtsn_error wtsn_db_migrate(wtsn_db *db) {
    char *err = NULL;
    int rc = sqlite3_exec(db->handle, SCHEMA, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        wtsn_log(WTSN_LOG_ERROR, "migrate failed: %s", err ? err : "unknown");
        sqlite3_free(err);
        return WTSN_ERR_DB;
    }
    /* existing databases created before the preemption column */
    run_migration(db, "ALTER TABLE qos_configs ADD COLUMN preemption INTEGER DEFAULT 0;");
    return WTSN_OK;
}

void wtsn_db_close(wtsn_db *db) {
    if (db && db->handle) {
        sqlite3_close(db->handle);
        db->handle = NULL;
    }
}
