#ifndef WTSN_TIMESYNC_MANAGER_H
#define WTSN_TIMESYNC_MANAGER_H

#include "common/common.h"
#include "db/db.h"
#include "db/db_timesync.h"
#include "mvc/event_bus.h"
#include "timesync/timesync.h"

#define WTSN_TIMESYNC_MANAGER_MODEL "timesync"

typedef struct wtsn_timesync_manager wtsn_timesync_manager;

wtsn_timesync_manager *wtsn_timesync_manager_create(wtsn_db *db, wtsn_event_bus *bus);
void wtsn_timesync_manager_destroy(wtsn_timesync_manager *m);

wtsn_error wtsn_timesync_manager_set_mode(wtsn_timesync_manager *m, wtsn_timesync_mode mode);
wtsn_error wtsn_timesync_manager_set_grandmaster(wtsn_timesync_manager *m, const char *gm_id);
const wtsn_timesync_status *wtsn_timesync_manager_status(wtsn_timesync_manager *m);
wtsn_error wtsn_timesync_manager_load(wtsn_timesync_manager *m);
wtsn_error wtsn_timesync_manager_record_report(wtsn_timesync_manager *m, const char *device_id,
                                             int64_t offset_ns, int64_t jitter_ns,
                                             int packet_count, int packet_loss);

#endif
