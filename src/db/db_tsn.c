#include "db/db_tsn.h"

#include "common/str_util.h"
#include <stdlib.h>
#include <string.h>

wtsn_error wtsn_db_tsn_save(wtsn_db *db, const wtsn_stream *s) {
    if (!db || !db->handle || !s) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO tsn_streams (stream_id,name,talker,vlan_id,max_latency_ns,"
        " max_interval_ns,priority,data_frame_prio,status,comment)"
        " VALUES (?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(stream_id) DO UPDATE SET name=excluded.name,"
        " talker=excluded.talker, vlan_id=excluded.vlan_id,"
        " max_latency_ns=excluded.max_latency_ns, max_interval_ns=excluded.max_interval_ns,"
        " priority=excluded.priority, data_frame_prio=excluded.data_frame_prio,"
        " status=excluded.status, comment=excluded.comment;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, s->stream_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, s->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s->talker, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, s->vlan_id);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)s->max_latency_ns);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)s->max_interval_ns);
    sqlite3_bind_int(st, 7, s->priority);
    sqlite3_bind_int(st, 8, s->data_frame_prio);
    sqlite3_bind_int(st, 9, (int)s->status);
    sqlite3_bind_text(st, 10, s->comment, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

    /* rewrite members */
    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(db->handle, "DELETE FROM tsn_stream_members WHERE stream_id=?;",
                           -1, &del, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(del, 1, s->stream_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_finalize(del);

    sqlite3_stmt *ins = NULL;
    const char *msql = "INSERT INTO tsn_stream_members (stream_id,role,device_id) VALUES (?,?,?);";
    if (sqlite3_prepare_v2(db->handle, msql, -1, &ins, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    /* talker */
    sqlite3_bind_text(ins, 1, s->stream_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, "talker", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, s->talker, -1, SQLITE_TRANSIENT);
    sqlite3_step(ins);
    sqlite3_reset(ins);
    /* listeners */
    if (s->listener_all) {
        sqlite3_bind_text(ins, 1, s->stream_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, "listener", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, WTSN_STREAM_ALL_LISTENERS, -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    for (size_t i = 0; i < s->listener_count; i++) {
        if (!s->listeners[i][0]) continue;
        sqlite3_bind_text(ins, 1, s->stream_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, "listener", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, s->listeners[i], -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    return WTSN_OK;
}

wtsn_error wtsn_db_tsn_load(wtsn_db *db, const char *stream_id, wtsn_stream *out) {
    if (!db || !db->handle || !stream_id || !out) return WTSN_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT stream_id,name,talker,vlan_id,max_latency_ns,max_interval_ns,"
        " priority,data_frame_prio,status,comment FROM tsn_streams WHERE stream_id=?;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, stream_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return WTSN_ERR_NOT_FOUND; }
    wtsn_strlcpy(out->stream_id, (const char *)sqlite3_column_text(st, 0), sizeof(out->stream_id));
    wtsn_strlcpy(out->name, (const char *)sqlite3_column_text(st, 1), sizeof(out->name));
    wtsn_strlcpy(out->talker, (const char *)sqlite3_column_text(st, 2), sizeof(out->talker));
    out->vlan_id = sqlite3_column_int(st, 3);
    out->max_latency_ns = (int64_t)sqlite3_column_int64(st, 4);
    out->max_interval_ns = (int64_t)sqlite3_column_int64(st, 5);
    out->priority = sqlite3_column_int(st, 6);
    out->data_frame_prio = sqlite3_column_int(st, 7);
    out->status = (wtsn_stream_status)sqlite3_column_int(st, 8);
    wtsn_strlcpy(out->comment, (const char *)sqlite3_column_text(st, 9), sizeof(out->comment));
    sqlite3_finalize(st);

    sqlite3_stmt *m = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "SELECT role,device_id FROM tsn_stream_members WHERE stream_id=?;",
        -1, &m, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(m, 1, stream_id, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(m) == SQLITE_ROW) {
        const char *role = (const char *)sqlite3_column_text(m, 0);
        const char *dev = (const char *)sqlite3_column_text(m, 1);
        if (strcmp(role, "listener") == 0) {
            if (strcmp(dev, WTSN_STREAM_ALL_LISTENERS) == 0) out->listener_all = 1;
            else if (out->listener_count < WTSN_MAX_DEVICES)
                wtsn_strlcpy(out->listeners[out->listener_count++], dev, WTSN_MAX_STR);
        }
    }
    sqlite3_finalize(m);
    return WTSN_OK;
}

void wtsn_db_tsn_for_each(wtsn_db *db, int (*cb)(const wtsn_stream *s, void *ud), void *ud) {
    if (!db || !db->handle || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, "SELECT stream_id FROM tsn_streams;",
                           -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        wtsn_stream s;
        if (wtsn_db_tsn_load(db, (const char *)sqlite3_column_text(st, 0), &s) == WTSN_OK) {
            int r = cb(&s, ud);
            if (r != 0) break;
        }
    }
    sqlite3_finalize(st);
}

wtsn_error wtsn_db_tsn_delete(wtsn_db *db, const char *stream_id) {
    if (!db || !db->handle || !stream_id) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, "DELETE FROM tsn_streams WHERE stream_id=?;",
                           -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, stream_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_stmt *m = NULL;
    if (sqlite3_prepare_v2(db->handle, "DELETE FROM tsn_stream_members WHERE stream_id=?;",
                           -1, &m, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(m, 1, stream_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(m);
    sqlite3_finalize(m);
    return WTSN_OK;
}

wtsn_error wtsn_db_tsn_set_status(wtsn_db *db, const char *stream_id, wtsn_stream_status st) {
    if (!db || !db->handle || !stream_id) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle, "UPDATE tsn_streams SET status=? WHERE stream_id=?;",
                           -1, &q, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_int(q, 1, (int)st);
    sqlite3_bind_text(q, 2, stream_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(q);
    sqlite3_finalize(q);
    return WTSN_OK;
}
