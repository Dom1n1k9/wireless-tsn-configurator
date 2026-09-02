#include "qos/qos_manager.h"

#include "db/db_qos.h"

#include "qos/qos.h"

#include "common/str_util.h"

#include "common/log.h"
#include "mvc/model.h"

#include <stdlib.h>
#include <string.h>

struct wtsn_qos_manager {
    wtsn_db *db;
    wtsn_event_bus *bus;
    wtsn_model model;
};

wtsn_qos_manager *wtsn_qos_manager_create(wtsn_db *db, wtsn_event_bus *bus) {
    if (!db || !bus) return NULL;
    wtsn_qos_manager *m = calloc(1, sizeof(wtsn_qos_manager));
    if (!m) return NULL;
    m->db = db;
    m->bus = bus;
    wtsn_model_init(&m->model, WTSN_QOS_MANAGER_MODEL, bus);
    return m;
}

void wtsn_qos_manager_destroy(wtsn_qos_manager *m) {
    free(m);
}

wtsn_error wtsn_qos_manager_configure(wtsn_qos_manager *m, const char *device_id,
                                      const wtsn_qos_config_model *cfg) {
    if (!m || !device_id || !cfg) return WTSN_ERR_INVALID_ARG;
    wtsn_error e = wtsn_qos_validate(cfg);
    if (e != WTSN_OK) return e;

    wtsn_qos_config q;
    memset(&q, 0, sizeof(q));
    wtsn_strlcpy(q.device_id, device_id, sizeof(q.device_id));
    q.priority = cfg->priority;
    q.traffic_class = (int)cfg->traffic_class;
    q.bandwidth_kbps = cfg->bandwidth_kbps;
    q.latency_ms = cfg->latency_ms;
    q.preemption = (int)cfg->preemption;
    wtsn_db_qos_save(m->db, &q);

    wtsn_model_notify(&m->model, "changed");
    return WTSN_OK;
}

wtsn_error wtsn_qos_manager_load_for_device(wtsn_qos_manager *m, const char *device_id, wtsn_qos_config *out) {
    if (!m || !device_id || !out) return WTSN_ERR_INVALID_ARG;
    return wtsn_db_qos_load(m->db, device_id, out);
}

wtsn_error wtsn_qos_manager_remove(wtsn_qos_manager *m, const char *device_id) {
    if (!m || !device_id) return WTSN_ERR_INVALID_ARG;
    wtsn_db_qos_delete(m->db, device_id);
    wtsn_model_notify(&m->model, "changed");
    return WTSN_OK;
}
