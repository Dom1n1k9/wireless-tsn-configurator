#include "tas/tas_manager.h"

#include "common/log.h"
#include "mvc/model.h"

#include <stdlib.h>
#include <string.h>

struct wtsn_tas_manager {
    wtsn_db *db;
    wtsn_event_bus *bus;
    wtsn_model model;
};

wtsn_tas_manager *wtsn_tas_manager_create(wtsn_db *db, wtsn_event_bus *bus) {
    if (!db || !bus) return NULL;
    wtsn_tas_manager *m = calloc(1, sizeof(wtsn_tas_manager));
    if (!m) return NULL;
    m->db = db;
    m->bus = bus;
    wtsn_model_init(&m->model, WTSN_TAS_MANAGER_MODEL, bus);
    return m;
}

void wtsn_tas_manager_destroy(wtsn_tas_manager *m) {
    free(m);
}

static void to_model(const wtsn_tas_schedule *src, wtsn_tas_schedule_model *dst) {
    memset(dst, 0, sizeof(*dst));
    wtsn_strlcpy(dst->id, src->id, sizeof(dst->id));
    wtsn_strlcpy(dst->name, src->name, sizeof(dst->name));
    dst->cycle_time_ns = src->cycle_time_ns;
    wtsn_strlcpy(dst->deploy_target, src->deploy_target, sizeof(dst->deploy_target));
    memcpy(&dst->gcl.entries, src->entries, sizeof(wtsn_gcl_entry) * src->entry_count);
    dst->gcl.entry_count = src->entry_count;
    dst->gcl.cycle_time_ns = src->cycle_time_ns;
}

static void to_db_schedule_copy(const wtsn_tas_schedule_model *src, wtsn_tas_schedule *dst) {
    memset(dst, 0, sizeof(*dst));
    wtsn_strlcpy(dst->id, src->id, sizeof(dst->id));
    wtsn_strlcpy(dst->name, src->name, sizeof(dst->name));
    dst->cycle_time_ns = src->cycle_time_ns;
    wtsn_strlcpy(dst->deploy_target, src->deploy_target, sizeof(dst->deploy_target));
    dst->entry_count = src->gcl.entry_count;
    dst->entries = (wtsn_gcl_entry *)src->gcl.entries;
}

wtsn_error wtsn_tas_manager_save(wtsn_tas_manager *m, const wtsn_tas_schedule_model *s) {
    if (!m || !s) return WTSN_ERR_INVALID_ARG;
    if (wtsn_tas_validate(s) != WTSN_OK) return WTSN_ERR_INVALID_ARG;
    wtsn_tas_schedule db_s;
    to_db_schedule_copy(s, &db_s);
    wtsn_db_tas_save(m->db, &db_s);
    wtsn_model_notify(&m->model, "schedule_changed");
    return WTSN_OK;
}

wtsn_error wtsn_tas_manager_load(wtsn_tas_manager *m, const char *id, wtsn_tas_schedule_model *out) {
    if (!m || !id || !out) return WTSN_ERR_INVALID_ARG;
    wtsn_tas_schedule db_s;
    memset(&db_s, 0, sizeof(db_s));
    wtsn_error e = wtsn_db_tas_load(m->db, id, &db_s);
    if (e != WTSN_OK) return e;
    to_model(&db_s, out);
    free(db_s.entries);
    return WTSN_OK;
}

wtsn_error wtsn_tas_manager_delete(wtsn_tas_manager *m, const char *id) {
    if (!m || !id) return WTSN_ERR_INVALID_ARG;
    wtsn_db_tas_delete(m->db, id);
    wtsn_model_notify(&m->model, "schedule_changed");
    return WTSN_OK;
}

wtsn_error wtsn_tas_manager_deploy(wtsn_tas_manager *m, const char *id) {
    if (!m || !id) return WTSN_ERR_INVALID_ARG;
    wtsn_tas_schedule_model s;
    memset(&s, 0, sizeof(s));
    wtsn_error e = wtsn_tas_manager_load(m, id, &s);
    if (e != WTSN_OK) return e;
    wtsn_model_notify_data(&m->model, "deployed", &s);
    return WTSN_OK;
}
