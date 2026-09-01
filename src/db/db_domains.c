#include "db/db_domains.h"

#include "common/str_util.h"

#include <string.h>

wtsn_error wtsn_db_domain_save(wtsn_db *db, const wtsn_domain *d) {
    if (!db || !db->handle || !d) return WTSN_ERR_INVALID_ARG;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO domains (id,name,description) VALUES (?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET name=excluded.name, description=excluded.description;";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, d->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, d->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, d->description, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? WTSN_OK : WTSN_ERR_DB;
}

wtsn_error wtsn_db_domain_delete(wtsn_db *db, const char *id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, "DELETE FROM domains WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
        return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}

void wtsn_db_domain_for_each(wtsn_db *db, wtsn_db_domain_cb cb, void *userdata) {
    if (!db || !db->handle || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, "SELECT id,name,description FROM domains;", -1, &st, NULL) != SQLITE_OK)
        return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        wtsn_domain d;
        memset(&d, 0, sizeof(d));
        wtsn_strlcpy(d.id, (const char *)sqlite3_column_text(st, 0), sizeof(d.id));
        wtsn_strlcpy(d.name, (const char *)sqlite3_column_text(st, 1), sizeof(d.name));
        wtsn_strlcpy(d.description, (const char *)sqlite3_column_text(st, 2), sizeof(d.description));
        cb(&d, userdata);
    }
    sqlite3_finalize(st);
}
