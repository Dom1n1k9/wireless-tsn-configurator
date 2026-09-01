#include "config_version/config_version_manager.h"

#include "common/log.h"
#include "db/db_config_versions.h"
#include "db/db_devices.h"
#include "db/db_qos.h"
#include "db/db_vlan.h"
#include "mvc/model.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct wtsn_config_version_manager {
    wtsn_db *db;
    wtsn_event_bus *bus;
    wtsn_model model;
};

static void append_field(char *buf, size_t cap, const char *fmt, ...) {
    size_t len = strlen(buf);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
}

static int collect_vlan(const wtsn_vlan_group *g, void *ud) {
    char *buf = (char *)ud;
    append_field(buf, 4096, "vlan:%s:%d ", g->id, g->vlan_id);
    return 0;
}

static void collect_device(const wtsn_device *d, void *ud) {
    char *buf = (char *)ud;
    append_field(buf, 4096, "device:%s:kind=%s:domain=%s ", d->id,
                 wtsn_device_kind_str(d->kind), d->domain[0] ? d->domain : "default");
}

/* Serialize the configuration scope into a canonical string used to compare
 * versions. For a specific device we include its QoS + VLAN membership +
 * stream role; for global we include all devices, VLAN groups and QoS rows. */
static void serialize_scope(wtsn_config_version_manager *m, const char *device_id, char *out,
                            size_t cap) {
    out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (device_id && device_id[0]) {
        if (sqlite3_prepare_v2(m->db->handle,
            "SELECT priority,bandwidth_kbps,latency_ms,preemption FROM qos_configs WHERE device_id=?;",
            -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, device_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                char row[128];
                snprintf(row, sizeof(row), "qos:p=%d:bw=%d:lat=%d:pre=%d ",
                         sqlite3_column_int(st, 0), sqlite3_column_int(st, 1),
                         sqlite3_column_int(st, 2), sqlite3_column_int(st, 3));
                append_field(out, cap, "%s", row);
            }
            sqlite3_finalize(st);
        }
        if (sqlite3_prepare_v2(m->db->handle,
            "SELECT gr.id,gr.vlan_id FROM vlan_members vm JOIN vlan_groups gr "
            "ON gr.id=vm.group_id WHERE vm.device_id=?;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, device_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                char row[128];
                snprintf(row, sizeof(row), "vlan:%s:%d ", (char *)sqlite3_column_text(st, 0),
                         sqlite3_column_int(st, 1));
                append_field(out, cap, "%s", row);
            }
            sqlite3_finalize(st);
        }
    } else {
        wtsn_db_device_for_each(m->db, collect_device, out);
        wtsn_db_vlan_group_for_each(m->db, collect_vlan, out);
        if (sqlite3_prepare_v2(m->db->handle,
            "SELECT device_id,priority,bandwidth_kbps,latency_ms,preemption FROM qos_configs;",
            -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                char row[128];
                snprintf(row, sizeof(row), "qos:%s:p=%d:bw=%d:lat=%d:pre=%d ",
                         (char *)sqlite3_column_text(st, 0), sqlite3_column_int(st, 1),
                         sqlite3_column_int(st, 2), sqlite3_column_int(st, 3),
                         sqlite3_column_int(st, 4));
                append_field(out, cap, "%s", row);
            }
            sqlite3_finalize(st);
        }
    }
}

wtsn_config_version_manager *wtsn_cfg_ver_manager_create(wtsn_db *db, wtsn_event_bus *bus) {
    if (!db || !bus) return NULL;
    wtsn_config_version_manager *m = calloc(1, sizeof(wtsn_config_version_manager));
    if (!m) return NULL;
    m->db = db;
    m->bus = bus;
    wtsn_model_init(&m->model, WTSN_CONFIG_VERSION_MODEL, bus);
    return m;
}

void wtsn_cfg_ver_manager_destroy(wtsn_config_version_manager *m) {
    free(m);
}

wtsn_error wtsn_cfg_ver_snapshot(wtsn_config_version_manager *m, const char *name,
                                 const char *device_id) {
    if (!m || !name) return WTSN_ERR_INVALID_ARG;
    char buf[4096];
    serialize_scope(m, device_id, buf, sizeof(buf));
    wtsn_db_config_version_add(m->db, name, device_id ? device_id : "", buf);
    wtsn_model_notify(&m->model, "changed");
    return WTSN_OK;
}

static wtsn_error restore_qos(wtsn_db *db, const char *tdev, int p, int bw, int lat, int pre) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
        "INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
        "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?);",
        -1, &st, NULL) != SQLITE_OK) return WTSN_ERR_DB;
    sqlite3_bind_text(st, 1, tdev, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, p);
    sqlite3_bind_int(st, 3, p);
    sqlite3_bind_int(st, 4, bw);
    sqlite3_bind_int(st, 5, lat);
    sqlite3_bind_int(st, 6, pre);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return WTSN_OK;
}

/* Restore the config tables from a version payload: whitespace-separated tokens of
 *   device:<id>:kind=<kind>:domain=<domain>
 *   vlan:<id>:<vlan_id>
 *   qos:<device>:p=<prio>:bw=<kbps>:lat=<ms>:pre=<pre>   (global)
 *   qos:p=<prio>:bw=<kbps>:lat=<ms>:pre=<pre>             (device scope)
 * Global rollbacks wipe the config tables and rebuild them from the payload. */
static wtsn_error restore_payload(wtsn_config_version_manager *m, const char *payload,
                                 const char *device_id) {
    wtsn_db *db = m->db;
    sqlite3_stmt *st = NULL;
    if (device_id && device_id[0]) {
        if (sqlite3_prepare_v2(db->handle, "DELETE FROM qos_configs WHERE device_id=?;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, device_id, -1, SQLITE_TRANSIENT);
            sqlite3_step(st); sqlite3_finalize(st);
        }
        if (sqlite3_prepare_v2(db->handle, "DELETE FROM vlan_members WHERE device_id=?;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, device_id, -1, SQLITE_TRANSIENT);
            sqlite3_step(st); sqlite3_finalize(st);
        }
    } else {
        if (sqlite3_prepare_v2(db->handle, "DELETE FROM qos_configs;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_step(st); sqlite3_finalize(st);
        }
        if (sqlite3_prepare_v2(db->handle, "DELETE FROM vlan_members;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_step(st); sqlite3_finalize(st);
        }
    }

    char buf[4096];
    wtsn_strlcpy(buf, payload ? payload : "", sizeof(buf));
    char *save = NULL;
    char *tok = strtok_r(buf, " ", &save);
    while (tok) {
        if (strncmp(tok, "qos:p=", 6) == 0) {
            int p = 0, bw = 0, lat = 0, pre = 0;
            if (sscanf(tok, "qos:p=%d:bw=%d:lat=%d:pre=%d", &p, &bw, &lat, &pre) >= 1) {
                const char *tdev = (device_id && device_id[0]) ? device_id : "default";
                restore_qos(db, tdev, p, bw, lat, pre);
            }
        } else if (strncmp(tok, "qos:", 4) == 0) {
            char tdev[WTSN_MAX_STR] = "";
            int p = 0, bw = 0, lat = 0, pre = 0;
            if (sscanf(tok, "qos:%255[^:]:p=%d:bw=%d:lat=%d:pre=%d",
                       tdev, &p, &bw, &lat, &pre) == 5) {
                restore_qos(db, tdev, p, bw, lat, pre);
            }
        } else if (strncmp(tok, "vlan:", 5) == 0) {
            char gid[WTSN_MAX_STR] = "";
            int vid = 0;
            if (sscanf(tok, "vlan:%255[^:]:%d", gid, &vid) == 2 && gid[0] && vid > 0) {
                if (sqlite3_prepare_v2(db->handle,
                    "INSERT OR REPLACE INTO vlan_groups(id,name,vlan_id) VALUES(?,?,?);",
                    -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, gid, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st, 2, gid, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(st, 3, vid);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
            }
        }
        tok = strtok_r(NULL, " ", &save);
    }
    return WTSN_OK;
}

wtsn_error wtsn_cfg_ver_rollback(wtsn_config_version_manager *m, int id) {
    if (!m) return WTSN_ERR_INVALID_ARG;
    wtsn_config_version v;
    if (wtsn_db_config_version_get(m->db, id, &v) != WTSN_OK) return WTSN_ERR_NOT_FOUND;
    wtsn_error e = restore_payload(m, v.payload, v.device_id);
    if (e != WTSN_OK) return e;
    wtsn_model_notify(&m->model, "changed");
    return WTSN_OK;
}

wtsn_error wtsn_cfg_ver_diff(wtsn_config_version_manager *m, int id_a, int id_b,
                             char *out, size_t out_size) {
    if (!m || !out || out_size == 0) return WTSN_ERR_INVALID_ARG;
    wtsn_config_version a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    wtsn_db_config_version_get(m->db, id_a, &a);
    wtsn_db_config_version_get(m->db, id_b, &b);
    if (strcmp(a.payload, b.payload) == 0) {
        snprintf(out, out_size, "no differences");
        return WTSN_OK;
    }
    snprintf(out, out_size, "config differs between v%d and v%d", id_a, id_b);
    return WTSN_OK;
}

int wtsn_cfg_ver_count(wtsn_config_version_manager *m) {
    return m ? wtsn_db_config_version_count(m->db) : 0;
}

void wtsn_cfg_ver_for_each(wtsn_config_version_manager *m, wtsn_db_config_version_cb cb,
                           void *ud) {
    if (m) wtsn_db_config_version_for_each(m->db, cb, ud);
}
