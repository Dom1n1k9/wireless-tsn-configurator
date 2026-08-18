#include "db/db_tas.h"

#include <stdlib.h>
#include <string.h>

#define WTSN_TAS_MAX_ENTRIES 128

wtsn_error wtsn_db_tas_save(wtsn_db *db, const wtsn_tas_schedule *s) {
    if (!db || !db->handle || !s) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO tas_schedules (id,name,cycle_time_ns,deploy_target) VALUES (?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET name=excluded.name,"
        " cycle_time_ns=excluded.cycle_time_ns, deploy_target=excluded.deploy_target;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, s->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, s->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)s->cycle_time_ns);
    sqlite3_bind_text(st, 4, s->deploy_target, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(db->handle, "DELETE FROM gcl_entries WHERE schedule_id=?;",
                           -1, &del, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(del, 1, s->id, -1, SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_finalize(del);

    for (size_t i = 0; i < s->entry_count; i++) {
        sqlite3_stmt *e = NULL;
        if (sqlite3_prepare_v2(db->handle,
            "INSERT INTO gcl_entries (schedule_id,index,gate_state,duration_ns)"
            " VALUES (?,?,?,?);", -1, &e, NULL) != SQLITE_OK) continue;
        sqlite3_bind_text(e, 1, s->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(e, 2, (int)i);
        sqlite3_bind_int(e, 3, (int)s->entries[i].gate_state);
        sqlite3_bind_int64(e, 4, (sqlite3_int64)s->entries[i].duration_ns);
        sqlite3_step(e);
        sqlite3_finalize(e);
    }
    return WTSN_OK;
}

wtsn_error wtsn_db_tas_load(wtsn_db *db, const char *id, wtsn_tas_schedule *out) {
    if (!db || !db->handle || !id || !out) return WTSN_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT id,name,cycle_time_ns,deploy_target FROM tas_schedules WHERE id=?;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return WTSN_ERR_NOT_FOUND; }
    wtsn_strlcpy(out->id, (const char *)sqlite3_column_text(st, 0), sizeof(out->id));
    wtsn_strlcpy(out->name, (const char *)sqlite3_column_text(st, 1), sizeof(out->name));
    out->cycle_time_ns = (int64_t)sqlite3_column_int64(st, 2);
    wtsn_strlcpy(out->deploy_target, (const char *)sqlite3_column_text(st, 3), sizeof(out->deploy_target));
    sqlite3_finalize(st);

    sqlite3_stmt *g = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "SELECT gate_state,duration_ns FROM gcl_entries WHERE schedule_id=? ORDER BY index;",
        -1, &g, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(g, 1, id, -1, SQLITE_TRANSIENT);
    out->entries = calloc(WTSN_TAS_MAX_ENTRIES, sizeof(wtsn_gcl_entry));
    while (sqlite3_step(g) == SQLITE_ROW && out->entry_count < WTSN_TAS_MAX_ENTRIES) {
        out->entries[out->entry_count].gate_state = (unsigned char)sqlite3_column_int(g, 0);
        out->entries[out->entry_count].duration_ns = (int64_t)sqlite3_column_int64(g, 1);
        out->entry_count++;
    }
    sqlite3_finalize(g);
    return WTSN_OK;
}

void wtsn_db_tas_for_each(wtsn_db *db, int (*cb)(const wtsn_tas_schedule *s, void *ud), void *ud) {
    if (!db || !db->handle || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, "SELECT id FROM tas_schedules;", -1, &st, NULL) != SQLITE_OK)
        return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        wtsn_tas_schedule s;
        if (wtsn_db_tas_load(db, (const char *)sqlite3_column_text(st, 0), &s) == WTSN_OK) {
            int r = cb(&s, ud);
            free(s.entries);
            if (r != 0) break;
        }
    }
    sqlite3_finalize(st);
}

wtsn_error wtsn_db_tas_delete(wtsn_db *db, const char *id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, "DELETE FROM tas_schedules WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}
