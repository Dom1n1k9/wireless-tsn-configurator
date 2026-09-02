#include "domain/domain_manager.h"

#include "db/db_domains.h"

#include "common/log.h"
#include "common/str_util.h"
#include "db/db_devices.h"
#include "mvc/model.h"

#include <stdlib.h>
#include <string.h>

struct wtsn_domain_manager {
    wtsn_db *db;
    wtsn_event_bus *bus;
    wtsn_model model;
};

wtsn_domain_manager *wtsn_domain_manager_create(wtsn_db *db, wtsn_event_bus *bus) {
    if (!db || !bus) return NULL;
    wtsn_domain_manager *m = calloc(1, sizeof(wtsn_domain_manager));
    if (!m) return NULL;
    m->db = db;
    m->bus = bus;
    wtsn_model_init(&m->model, WTSN_DOMAIN_MANAGER_MODEL, bus);
    wtsn_domain def;
    memset(&def, 0, sizeof(def));
    wtsn_strlcpy(def.id, "default", sizeof(def.id));
    wtsn_strlcpy(def.name, "Default", sizeof(def.name));
    wtsn_strlcpy(def.description, "Global default domain", sizeof(def.description));
    wtsn_db_domain_save(db, &def);
    return m;
}

void wtsn_domain_manager_destroy(wtsn_domain_manager *m) {
    free(m);
}

wtsn_error wtsn_domain_manager_save(wtsn_domain_manager *m, const wtsn_domain *d) {
    if (!m || !d) return WTSN_ERR_INVALID_ARG;
    wtsn_db_domain_save(m->db, d);
    wtsn_model_notify(&m->model, "changed");
    return WTSN_OK;
}

wtsn_error wtsn_domain_manager_delete(wtsn_domain_manager *m, const char *id) {
    if (!m || !id) return WTSN_ERR_INVALID_ARG;
    if (strcmp(id, "default") == 0) return WTSN_ERR_INVALID_ARG;
    wtsn_db_domain_delete(m->db, id);
    /* re-home devices that referenced this domain back to default */
    wtsn_db *db = m->db;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, "UPDATE devices SET domain='default' WHERE domain=?;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    wtsn_model_notify(&m->model, "changed");
    return WTSN_OK;
}

void wtsn_domain_manager_for_each(wtsn_domain_manager *m, wtsn_db_domain_cb cb, void *ud) {
    if (m) wtsn_db_domain_for_each(m->db, cb, ud);
}

wtsn_error wtsn_domain_manager_assign_device(wtsn_domain_manager *m, const char *device_id,
                                            const char *domain_id) {
    if (!m || !device_id || !domain_id) return WTSN_ERR_INVALID_ARG;
    wtsn_db *db = m->db;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, "UPDATE devices SET domain=? WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, domain_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, device_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    wtsn_model_notify(&m->model, "changed");
    return WTSN_OK;
}
