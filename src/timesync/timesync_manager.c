#include "timesync/timesync_manager.h"

#include "common/log.h"
#include "mvc/model.h"

#include <stdlib.h>
#include <string.h>

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
