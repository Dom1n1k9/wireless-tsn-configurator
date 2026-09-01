#ifndef WTSN_DB_TIMESYNC_REPORT_H
#define WTSN_DB_TIMESYNC_REPORT_H

#include "db/db.h"
#include "timesync/timesync.h"

#include <time.h>

typedef struct {
    char device_id[WTSN_MAX_STR];
    time_t ts;
    int64_t offset_ns;
    int64_t jitter_ns;
    int packet_count;
    int packet_loss;
    char status[32];
} wtsn_timesync_report;

wtsn_error wtsn_db_timesync_report_insert(wtsn_db *db, const wtsn_timesync_report *r);
typedef void (*wtsn_db_report_cb)(const wtsn_timesync_report *r, void *userdata);
void wtsn_db_timesync_report_for_each(wtsn_db *db, const char *device_id,
                                     int limit, wtsn_db_report_cb cb, void *userdata);
wtsn_error wtsn_db_timesync_report_prune(wtsn_db *db, int keep);

#endif
