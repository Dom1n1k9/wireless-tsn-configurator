#ifndef WTSN_DB_TAS_H
#define WTSN_DB_TAS_H

#include "db/db.h"
#include "tas/gcl.h"

typedef struct {
    char id[WTSN_MAX_STR];
    char name[WTSN_MAX_STR];
    int64_t cycle_time_ns;
    char deploy_target[WTSN_MAX_STR];
    unsigned char gate_state;
    wtsn_gcl_entry *entries;
    size_t entry_count;
} wtsn_tas_schedule;

wtsn_error wtsn_db_tas_save(wtsn_db *db, const wtsn_tas_schedule *s);
wtsn_error wtsn_db_tas_load(wtsn_db *db, const char *id, wtsn_tas_schedule *out);
void wtsn_db_tas_for_each(wtsn_db *db, int (*cb)(const wtsn_tas_schedule *s, void *ud), void *ud);
wtsn_error wtsn_db_tas_delete(wtsn_db *db, const char *id);

#endif
