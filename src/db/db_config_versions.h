#ifndef WTSN_DB_CONFIG_VERSIONS_H
#define WTSN_DB_CONFIG_VERSIONS_H

#include "db/db.h"

#include <time.h>

typedef struct {
    int id;
    char name[WTSN_MAX_STR];
    char device_id[WTSN_MAX_STR];
    time_t created_at;
    char payload[4096];
} wtsn_config_version;

wtsn_error wtsn_db_config_version_add(wtsn_db *db, const char *name,
                                      const char *device_id, const char *payload);
wtsn_error wtsn_db_config_version_get(wtsn_db *db, int id, wtsn_config_version *out);
typedef void (*wtsn_db_config_version_cb)(const wtsn_config_version *v, void *userdata);
void wtsn_db_config_version_for_each(wtsn_db *db, wtsn_db_config_version_cb cb, void *userdata);
int wtsn_db_config_version_count(wtsn_db *db);

#endif
