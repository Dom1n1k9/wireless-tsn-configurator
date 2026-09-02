#include "timesync/timesync_manager.h"

#include "db/db_timesync.h"

#include "timesync/timesync.h"

#include "common/str_util.h"

#include "common/log.h"
#include "db/db_timesync_report.h"
#include "mvc/model.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct wtsn_timesync_manager {
    wtsn_db *db;
    wtsn_event_bus *bus;
    wtsn_model model;
    wtsn_timesync_status status;
};

wtsn_timesync_manager *wtsn_timesync_manager_create(wtsn_db *db, wtsn_event_bus *bus) {
    if (!db || !bus) return NULL;
    wtsn_timesync_manager *m = calloc(1, sizeof(wtsn_timesync_manager));
    if (!m) return NULL;
    m->db = db;
    m->bus = bus;
    wtsn_model_init(&m->model, WTSN_TIMESYNC_MANAGER_MODEL, bus);
    wtsn_timesync_manager_load(m);
    return m;
}

void wtsn_timesync_manager_destroy(wtsn_timesync_manager *m) {
    free(m);
}

wtsn_error wtsn_timesync_manager_load(wtsn_timesync_manager *m) {
    if (!m) return WTSN_ERR_INVALID_ARG;
    if (wtsn_db_timesync_load(m->db, &m->status) != WTSN_OK) {
        memset(&m->status, 0, sizeof(m->status));
        m->status.mode = WTSN_TIMESYNC_DISABLED;
        wtsn_strlcpy(m->status.protocol, "gptp", sizeof(m->status.protocol));
    }
    return WTSN_OK;
}

wtsn_error wtsn_timesync_manager_set_mode(wtsn_timesync_manager *m, wtsn_timesync_mode mode) {
    if (!m) return WTSN_ERR_INVALID_ARG;
    wtsn_timesync_validate_mode(mode);
    m->status.mode = mode;
    wtsn_db_timesync_save(m->db, &m->status);
    wtsn_model_notify(&m->model, "changed");
    return WTSN_OK;
}

wtsn_error wtsn_timesync_manager_set_grandmaster(wtsn_timesync_manager *m, const char *gm_id) {
    if (!m || !gm_id) return WTSN_ERR_INVALID_ARG;
    wtsn_strlcpy(m->status.grandmaster, gm_id, sizeof(m->status.grandmaster));
    wtsn_db_timesync_save(m->db, &m->status);
    wtsn_model_notify(&m->model, "changed");
    return WTSN_OK;
}

const wtsn_timesync_status *wtsn_timesync_manager_status(wtsn_timesync_manager *m) {
    return m ? &m->status : NULL;
}

wtsn_error wtsn_timesync_manager_record_report(wtsn_timesync_manager *m, const char *device_id,
                                           int64_t offset_ns, int64_t jitter_ns,
                                           int packet_count, int packet_loss) {
    if (!m || !device_id) return WTSN_ERR_INVALID_ARG;
    wtsn_timesync_report r;
    memset(&r, 0, sizeof(r));
    wtsn_strlcpy(r.device_id, device_id, sizeof(r.device_id));
    r.offset_ns = offset_ns;
    r.jitter_ns = jitter_ns;
    r.packet_count = packet_count;
    r.packet_loss = packet_loss;
    wtsn_strlcpy(r.status, packet_loss > 0 ? "degraded" : "in_sync", sizeof(r.status));
    wtsn_db_timesync_report_insert(m->db, &r);
    wtsn_db_timesync_report_prune(m->db, 1000);
    /* update the active per-device summary shown to the user */
    wtsn_strlcpy(m->status.report_device, device_id, sizeof(m->status.report_device));
    m->status.report_offset_ns = offset_ns;
    m->status.report_jitter_ns = jitter_ns;
    m->status.report_packet_count = packet_count;
    m->status.report_packet_loss = packet_loss;
    wtsn_model_notify_data(&m->model, "report", &m->status);
    return WTSN_OK;
}
