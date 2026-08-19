#include "device/device_manager.h"

#include "common/log.h"
#include "mvc/model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEVICE_MODEL_NAME "device"
#define MAX_DB_DEVICES 512

struct wtsn_device_manager {
    wtsn_db *db;
    wtsn_event_bus *bus;
    wtsn_plugin_manager *plugins;
    wtsn_model model;
    wtsn_device devices[MAX_DB_DEVICES];
    size_t count;
};

static void collect_devices(const wtsn_device *dev, void *ud) {
    wtsn_device_manager *m = (wtsn_device_manager *)ud;
    if (m->count < MAX_DB_DEVICES) {
        m->devices[m->count++] = *dev;
    }
}

static void publish_device_event(wtsn_device_manager *m, const char *event,
                                 const wtsn_device *dev) {
    if (!m) return;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", event);
    wtsn_model_notify_data(&m->model, buf, (void *)dev);
}

wtsn_device_manager *wtsn_device_manager_create(wtsn_db *db, wtsn_event_bus *bus,
                                                 wtsn_plugin_manager *plugins) {
    if (!db || !bus) return NULL;
    wtsn_device_manager *m = calloc(1, sizeof(wtsn_device_manager));
    if (!m) return NULL;
    m->db = db;
    m->bus = bus;
    m->plugins = plugins;
    wtsn_model_init(&m->model, DEVICE_MODEL_NAME, bus);
    wtsn_device_manager_restore(m);
    return m;
}

void wtsn_device_manager_destroy(wtsn_device_manager *m) {
    if (!m) return;
    for (size_t i = 0; i < m->count; i++) {
        wtsn_db_device_upsert(m->db, &m->devices[i]);
    }
    free(m);
}

const wtsn_device *wtsn_device_manager_get(wtsn_device_manager *m, const char *id) {
    if (!m || !id) return NULL;
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->devices[i].id, id) == 0) return &m->devices[i];
    }
    return NULL;
}

size_t wtsn_device_manager_count(wtsn_device_manager *m) {
    return m ? m->count : 0;
}

void wtsn_device_manager_for_each(wtsn_device_manager *m,
                                  void (*cb)(const wtsn_device *dev, void *ud), void *ud) {
    if (!m || !cb) return;
    for (size_t i = 0; i < m->count; i++) cb(&m->devices[i], ud);
}

wtsn_error wtsn_device_manager_upsert(wtsn_device_manager *m, const wtsn_device *dev) {
    if (!m || !dev) return WTSN_ERR_INVALID_ARG;
    wtsn_device copy = *dev;
    copy.last_seen = dev->last_seen ? dev->last_seen : time(NULL);

    bool exists = false;
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->devices[i].id, copy.id) == 0) {
            m->devices[i] = copy;
            exists = true;
            break;
        }
    }
    if (!exists) {
        if (m->count >= MAX_DB_DEVICES) return WTSN_ERR_BUSY;
        m->devices[m->count++] = copy;
    }

    wtsn_db_device_upsert(m->db, &copy);
    publish_device_event(m, exists ? "updated" : "discovered", &copy);
    return WTSN_OK;
}

wtsn_error wtsn_device_manager_remove(wtsn_device_manager *m, const char *id) {
    if (!m || !id) return WTSN_ERR_INVALID_ARG;
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->devices[i].id, id) == 0) {
            wtsn_device removed = m->devices[i];
            memmove(&m->devices[i], &m->devices[i+1],
                    (m->count - i - 1) * sizeof(wtsn_device));
            m->count--;
            wtsn_db_device_delete(m->db, id);
            publish_device_event(m, "removed", &removed);
            return WTSN_OK;
        }
    }
    return WTSN_ERR_NOT_FOUND;
}

void wtsn_device_manager_restore(wtsn_device_manager *m) {
    for (size_t i = 0; i < MAX_DB_DEVICES; i++) m->devices[i].id[0] = '\0';
    m->count = 0;
    wtsn_db_device_for_each(m->db, collect_devices, m);
    wtsn_log(WTSN_LOG_INFO, "restored %zu devices from database", m->count);
}

void wtsn_device_manager_mark_offline_after(wtsn_device_manager *m, time_t threshold) {
    if (!m) return;
    for (size_t i = 0; i < m->count; i++) {
        wtsn_device *d = &m->devices[i];
        if (d->last_seen < threshold && d->status != WTSN_DEVICE_ERROR) {
            d->status = WTSN_DEVICE_OFFLINE;
            wtsn_db_device_set_status(m->db, d->id, d->status);
            publish_device_event(m, "status", d);
        }
    }
}

static void collect_plugin_devices(wtsn_plugin *p, void *ud) {
    wtsn_device_manager *m = (wtsn_device_manager *)ud;
    wtsn_device out[WTSN_MAX_DEVICES];
    memset(out, 0, sizeof(out));
    int count = 0;
    if (p->discover && p->discover(p, out, WTSN_MAX_DEVICES, &count) == WTSN_OK) {
        for (int i = 0; i < count; i++) {
            wtsn_device_manager_upsert(m, &out[i]);
        }
    }
}

wtsn_error wtsn_device_manager_discover_once(wtsn_device_manager *m) {
    if (!m || !m->plugins) return WTSN_ERR_INVALID_ARG;
    for (size_t i = 0; i < wtsn_plugin_manager_count(m->plugins); i++) {
        collect_plugin_devices(wtsn_plugin_manager_get(m->plugins, i), m);
    }
    return WTSN_OK;
}
