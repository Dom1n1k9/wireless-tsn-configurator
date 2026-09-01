#include "db/db_timesync_report.h"

#include "common/str_util.h"

#include <string.h>
#include <time.h>

wtsn_error wtsn_db_timesync_report_insert(wtsn_db *db, const wtsn_timesync_report *r) {
    if (!db || !db->handle || !r) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO timesync_reports (device_id,ts,offset_ns,jitter_ns,packet_count,packet_loss,status)"
        " VALUES (?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, r->device_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)(r->ts ? r->ts : time(NULL)));
    sqlite3_bind_int64(st, 3, (sqlite3_int64)r->offset_ns);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)r->jitter_ns);
    sqlite3_bind_int(st, 5, r->packet_count);
    sqlite3_bind_int(st, 6, r->packet_loss);
    sqlite3_bind_text(st, 7, r->status, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}

void wtsn_db_timesync_report_for_each(wtsn_db *db, const char *device_id,
                                     int limit, wtsn_db_report_cb cb, void *userdata) {
    if (!db || !db->handle || !cb) return;
    sqlite3_stmt *st = NULL;
    if (limit <= 0) limit = 50;
    const char *sql = device_id && device_id[0]
        ? "SELECT device_id,ts,offset_ns,jitter_ns,packet_count,packet_loss,status "
          "FROM timesync_reports WHERE device_id=? ORDER BY id DESC LIMIT ?;"
        : "SELECT device_id,ts,offset_ns,jitter_ns,packet_count,packet_loss,status "
          "FROM timesync_reports ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return;
    int p = 1;
    if (device_id && device_id[0]) sqlite3_bind_text(st, p++, device_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, p, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        wtsn_timesync_report r;
        memset(&r, 0, sizeof(r));
        wtsn_strlcpy(r.device_id, (const char *)sqlite3_column_text(st, 0), sizeof(r.device_id));
        r.ts = (time_t)sqlite3_column_int64(st, 1);
        r.offset_ns = (int64_t)sqlite3_column_int64(st, 2);
        r.jitter_ns = (int64_t)sqlite3_column_int64(st, 3);
        r.packet_count = sqlite3_column_int(st, 4);
        r.packet_loss = sqlite3_column_int(st, 5);
        wtsn_strlcpy(r.status, (const char *)sqlite3_column_text(st, 6), sizeof(r.status));
        cb(&r, userdata);
    }
    sqlite3_finalize(st);
}

wtsn_error wtsn_db_timesync_report_prune(wtsn_db *db, int keep) {
    if (!db || !db->handle) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "DELETE FROM timesync_reports WHERE id NOT IN "
        "(SELECT id FROM timesync_reports ORDER BY id DESC LIMIT ?);",
        -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_int(st, 1, keep);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}
