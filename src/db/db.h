#ifndef WTSN_DB_H
#define WTSN_DB_H

#include "common/common.h"

#include <sqlite3.h>

typedef struct wtsn_db {
    sqlite3 *handle;
    char path[WTSN_MAX_STR];
} wtsn_db;

wtsn_error wtsn_db_open(wtsn_db *db, const char *path);
void wtsn_db_close(wtsn_db *db);
wtsn_error wtsn_db_migrate(wtsn_db *db);

#endif
