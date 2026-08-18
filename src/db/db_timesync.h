#ifndef WTSN_DB_TIMESYNC_H
#define WTSN_DB_TIMESYNC_H

#include "db/db.h"
#include "timesync/timesync.h"

wtsn_error wtsn_db_timesync_save(wtsn_db *db, const wtsn_timesync_status *s);
wtsn_error wtsn_db_timesync_load(wtsn_db *db, wtsn_timesync_status *out);

#endif
