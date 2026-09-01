#include "config_version/config_version_manager.h"

#include "common/log.h"
#include "common/str_util.h"
#include "db/db_config_versions.h"
#include "db/db_devices.h"
#include "db/db_qos.h"
#include "db/db_tas.h"
#include "db/db_tsn.h"
#include "db/db_vlan.h"
#include "mvc/model.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define CFG_PAYLOAD_MAX 65536

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

/* ---- serialization ------------------------------------------------------ */

static int collect_vlan(const wtsn_vlan_group *g, void *ud) {
    char *buf = (char *)ud;
    append_field(buf, CFG_PAYLOAD_MAX, "vlan:%s:%d ", g->id, g->vlan_id);
    return 0;
}

static void collect_vlan_member(const wtsn_vlan_member *m, void *ud) {
    char *buf = (char *)ud;
    append_field(buf, CFG_PAYLOAD_MAX, "vmem:%s:%s ", m->group_id, m->device_id);
}

static void collect_device(const wtsn_device *d, void *ud) {
    char *buf = (char *)ud;
    append_field(buf, CFG_PAYLOAD_MAX, "device:%s:kind=%s:domain=%s ", d->id,
                 wtsn_device_kind_str(d->kind), d->domain[0] ? d->domain : "default");
}

static int collect_stream(const wtsn_stream *s, void *ud) {
    char *buf = (char *)ud;
    append_field(buf, CFG_PAYLOAD_MAX, "stream:%s:%s:vlan=%d:prio=%d:lat=%lld:iv=%lld:st=%d ",
                s->stream_id, s->talker, s->vlan_id, s->priority,
                (long long)s->max_latency_ns, (long long)s->max_interval_ns, (int)s->status);
    for (size_t i = 0; i < s->listener_count; i++) {
        append_field(buf, CFG_PAYLOAD_MAX, "streammem:%s:%s ", s->stream_id, s->listeners[i]);
    }
    if (s->listener_all)
        append_field(buf, CFG_PAYLOAD_MAX, "streammem:%s:* ", s->stream_id);
    return 0;
}

static int collect_tas(const wtsn_tas_schedule *s, void *ud) {
    char *buf = (char *)ud;
    append_field(buf, CFG_PAYLOAD_MAX, "tas:%s:cycle=%lld:tgt=%s ",
                 s->id, (long long)s->cycle_time_ns, s->deploy_target);
    for (size_t i = 0; i < s->entry_count; i++) {
        append_field(buf, CFG_PAYLOAD_MAX, "gcl:%s:%d:%d:%lld ",
                   s->id, (int)i, (int)s->entries[i].gate_state,
                   (long long)s->entries[i].duration_ns);
    }
    return 0;
}

static void serialize_global(wtsn_config_version_manager *m, char *out, size_t cap) {
    out[0] = '\0';
    sqlite3_stmt *st = NULL;
    wtsn_db_device_for_each(m->db, collect_device, out);
    wtsn_db_vlan_group_for_each(m->db, collect_vlan, out);
    wtsn_db_vlan_member_for_each_all(m->db, collect_vlan_member, out);
    wtsn_db_tsn_for_each(m->db, collect_stream, out);
    wtsn_db_tas_for_each(m->db, collect_tas, out);
    if (sqlite3_prepare_v2(m->db->handle,
        "SELECT device_id,priority,bandwidth_kbps,latency_ms,preemption FROM qos_configs;",
        -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            append_field(out, cap, "qos:%s:p=%d:bw=%d:lat=%d:pre=%d ",
                       (char *)sqlite3_column_text(st, 0), sqlite3_column_int(st, 1),
                       sqlite3_column_int(st, 2), sqlite3_column_int(st, 3),
                       sqlite3_column_int(st, 4));
        }
        sqlite3_finalize(st);
    }
    append_field(out, cap, "pre:%s:emac=%s:pmac=%s ",
                "preemption", "", "");
}

static void serialize_device(wtsn_config_version_manager *m, const char *device_id,
                          char *out, size_t cap) {
    out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(m->db->handle,
        "SELECT priority,bandwidth_kbps,latency_ms,preemption FROM qos_configs WHERE device_id=?;",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, device_id, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            append_field(out, cap, "qos:%s:p=%d:bw=%d:lat=%d:pre=%d ", device_id,
                       sqlite3_column_int(st, 0), sqlite3_column_int(st, 1),
                       sqlite3_column_int(st, 2), sqlite3_column_int(st, 3));
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(m->db->handle,
        "SELECT group_id FROM vlan_members WHERE device_id=?;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, device_id, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            append_field(out, cap, "vmem:%s:%s ", (char *)sqlite3_column_text(st, 0), device_id);
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(m->db->handle,
        "SELECT stream_id FROM tsn_stream_members WHERE device_id=?;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, device_id, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            append_field(out, cap, "streammem:%s:%s ", (char *)sqlite3_column_text(st, 0),
                       device_id);
        }
        sqlite3_finalize(st);
    }
}

static void serialize_scope(wtsn_config_version_manager *m, const char *device_id, char *out,
                          size_t cap) {
    if (device_id && device_id[0]) serialize_device(m, device_id, out, cap);
    else serialize_global(m, out, cap);
}

/* ---- restore ----------------------------------------------------------- */

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

static void exec_del(wtsn_db *db, const char *sql, const char *arg) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) return;
    if (arg) sqlite3_bind_text(st, 1, arg, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Restore config tables from a version payload token stream. Global rollbacks
 * wipe the config tables first; device-scoped rollbacks wipe only that device. */
static wtsn_error restore_payload(wtsn_config_version_manager *m, const char *payload,
                                 const char *device_id) {
    wtsn_db *db = m->db;
    if (device_id && device_id[0]) {
        exec_del(db, "DELETE FROM qos_configs WHERE device_id=?;", device_id);
        exec_del(db, "DELETE FROM vlan_members WHERE device_id=?;", device_id);
        exec_del(db, "DELETE FROM tsn_stream_members WHERE device_id=?;", device_id);
    } else {
        exec_del(db, "DELETE FROM qos_configs;", NULL);
        exec_del(db, "DELETE FROM vlan_members;", NULL);
        exec_del(db, "DELETE FROM vlan_groups;", NULL);
        exec_del(db, "DELETE FROM tsn_streams;", NULL);
        exec_del(db, "DELETE FROM tsn_stream_members;", NULL);
        exec_del(db, "DELETE FROM tas_schedules;", NULL);
        exec_del(db, "DELETE FROM gcl_entries;", NULL);
    }

    char *buf = malloc(CFG_PAYLOAD_MAX);
    if (!buf) return WTSN_ERR_NO_MEMORY;
    wtsn_strlcpy(buf, payload ? payload : "", CFG_PAYLOAD_MAX);
    char *save = NULL;
    char *tok = strtok_r(buf, " ", &save);
    sqlite3_stmt *st = NULL;
    while (tok) {
        if (strncmp(tok, "qos:p=", 6) == 0) {
            int p = 0, bw = 0, lat = 0, pre = 0;
            if (sscanf(tok, "qos:p=%d:bw=%d:lat=%d:pre=%d", &p, &bw, &lat, &pre) >= 1) {
                restore_qos(db, (device_id && device_id[0]) ? device_id : "default",
                           p, bw, lat, pre);
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
        } else if (strncmp(tok, "vmem:", 5) == 0) {
            char gid[WTSN_MAX_STR] = "", dev[WTSN_MAX_STR] = "";
            if (sscanf(tok, "vmem:%255[^:]:%255[^:]", gid, dev) == 2) {
                if (sqlite3_prepare_v2(db->handle,
                    "INSERT OR IGNORE INTO vlan_members(group_id,device_id) VALUES(?,?);",
                    -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, gid, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st, 2, dev, -1, SQLITE_TRANSIENT);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
            }
        } else if (strncmp(tok, "stream:", 7) == 0) {
            char sid[WTSN_MAX_STR] = "", talker[WTSN_MAX_STR] = "";
            int vlan = 0, prio = 0, sts = 0;
            long long lat = 0, iv = 0;
            if (sscanf(tok, "stream:%255[^:]:%255[^:]:vlan=%d:prio=%d:lat=%lld:iv=%lld:st=%d",
                       sid, talker, &vlan, &prio, &lat, &iv, &sts) >= 4) {
                if (sqlite3_prepare_v2(db->handle,
                    "INSERT OR REPLACE INTO tsn_streams(stream_id,name,talker,vlan_id,"
                    "max_latency_ns,max_interval_ns,priority,data_frame_prio,status,comment)"
                    " VALUES(?,?,?,?,?,?,?,?,?,'');", -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, sid, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st, 2, sid, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st, 3, talker, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(st, 4, vlan);
                    sqlite3_bind_int64(st, 5, (sqlite3_int64)lat);
                    sqlite3_bind_int64(st, 6, (sqlite3_int64)iv);
                    sqlite3_bind_int(st, 7, prio);
                    sqlite3_bind_int(st, 8, prio);
                    sqlite3_bind_int(st, 9, sts);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
            }
        } else if (strncmp(tok, "streammem:", 10) == 0) {
            char sid[WTSN_MAX_STR] = "", dev[WTSN_MAX_STR] = "";
            if (sscanf(tok, "streammem:%255[^:]:%255[^:]", sid, dev) == 2) {
                const char *role = (strcmp(dev, "*") == 0) ? "listener" : "listener";
                if (sqlite3_prepare_v2(db->handle,
                    "INSERT OR IGNORE INTO tsn_stream_members(stream_id,role,device_id) VALUES(?,?,?);",
                    -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, sid, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st, 2, role, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st, 3, dev, -1, SQLITE_TRANSIENT);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
            }
        } else if (strncmp(tok, "tas:", 4) == 0) {
            char tid[WTSN_MAX_STR] = "", tgt[WTSN_MAX_STR] = "";
            long long cyc = 0;
            if (sscanf(tok, "tas:%255[^:]:cycle=%lld:tgt=%255s", tid, &cyc, tgt) >= 2) {
                if (sqlite3_prepare_v2(db->handle,
                    "INSERT OR REPLACE INTO tas_schedules(id,name,cycle_time_ns,deploy_target)"
                    " VALUES(?,?,?,?);", -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st, 2, tid, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(st, 3, (sqlite3_int64)cyc);
                    sqlite3_bind_text(st, 4, tgt, -1, SQLITE_TRANSIENT);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
            }
        } else if (strncmp(tok, "gcl:", 4) == 0) {
            char tid[WTSN_MAX_STR] = "";
            int idx = 0, gs = 0;
            long long dur = 0;
            if (sscanf(tok, "gcl:%255[^:]:%d:%d:%lld", tid, &idx, &gs, &dur) == 4) {
                if (sqlite3_prepare_v2(db->handle,
                    "INSERT OR REPLACE INTO gcl_entries(schedule_id,index,gate_state,duration_ns)"
                    " VALUES(?,?,?,?);", -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(st, 2, idx);
                    sqlite3_bind_int(st, 3, gs);
                    sqlite3_bind_int64(st, 4, (sqlite3_int64)dur);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
            }
        }
        tok = strtok_r(NULL, " ", &save);
    }
    free(buf);
    return WTSN_OK;
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
    char *buf = malloc(CFG_PAYLOAD_MAX);
    if (!buf) return WTSN_ERR_NO_MEMORY;
    serialize_scope(m, device_id, buf, CFG_PAYLOAD_MAX);
    wtsn_db_config_version_add(m->db, name, device_id ? device_id : "", buf);
    free(buf);
    wtsn_model_notify(&m->model, "changed");
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

/* Row-level diff: compare canonical token sets and list the rows that differ. */
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

    /* tokenise into line-per-row and compare */
    size_t off = 0;
    char *pa = strdup(a.payload), *pb = strdup(b.payload);
    int w = 10, h = 12;
    (void)w; (void)h;
    int maxl = (int)(out_size / (w + h));
    int an = 0, bn = 0;
    const char *arow[1024];
    const char *brow[1024];
    char *saveA = NULL, *saveB = NULL;
    char *ta = strtok_r(pa, " ", &saveA);
    while (ta && an < 1024) { arow[an++] = ta; ta = strtok_r(NULL, " ", &saveA); }
    char *tb = strtok_r(pb, " ", &saveB);
    while (tb && bn < 1024) { brow[bn++] = tb; tb = strtok_r(NULL, " ", &saveB); }

    /* mark differences line-by-line */
    int shown = 0;
    int amax = maxl / 2, bmax = maxl / 2;
    for (int i = 0; i < an && shown < amax; i++) {
        int found = 0;
        for (int j = 0; j < bn; j++) if (strcmp(arow[i], brow[j]) == 0) { found = 1; break; }
        if (!found) { off += (size_t)snprintf(out + off, out_size - off, "- %s\n", arow[i]); shown++; }
    }
    for (int j = 0; j < bn && shown < maxl; j++) {
        int found = 0;
        for (int i = 0; i < an; i++) if (strcmp(arow[i], brow[j]) == 0) { found = 1; break; }
        if (!found) { off += (size_t)snprintf(out + off, out_size - off, "+ %s\n", brow[j]); shown++; }
    }
    if (off == 0) snprintf(out, out_size, "versions differ (v%d vs v%d)", id_a, id_b);
    free(pa); free(pb);
    return WTSN_OK;
}

int wtsn_cfg_ver_count(wtsn_config_version_manager *m) {
    return m ? wtsn_db_config_version_count(m->db) : 0;
}

void wtsn_cfg_ver_for_each(wtsn_config_version_manager *m, wtsn_db_config_version_cb cb,
                           void *ud) {
    if (m) wtsn_db_config_version_for_each(m->db, cb, ud);
}
