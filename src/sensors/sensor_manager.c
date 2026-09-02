#include "sensors/sensor_manager.h"

#include "db/db_sensors.h"

#include "common/str_util.h"

#include "common/log.h"
#include "mvc/model.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SENSORS 1024

struct wtsn_sensor_manager {
    wtsn_db *db;
    wtsn_event_bus *bus;
    wtsn_model model;
    wtsn_sensor sensors[MAX_SENSORS];
    size_t count;
};

static void collect(const wtsn_sensor *s, void *ud) {
    wtsn_sensor_manager *m = (wtsn_sensor_manager *)ud;
    if (m->count < MAX_SENSORS) m->sensors[m->count++] = *s;
}

wtsn_sensor_manager *wtsn_sensor_manager_create(wtsn_db *db, wtsn_event_bus *bus) {
    if (!db || !bus) return NULL;
    wtsn_sensor_manager *m = calloc(1, sizeof(wtsn_sensor_manager));
    if (!m) return NULL;
    m->db = db;
    m->bus = bus;
    wtsn_model_init(&m->model, WTSN_SENSOR_MANAGER_MODEL, bus);
    wtsn_db_sensor_for_each(db, collect, m);
    return m;
}

void wtsn_sensor_manager_destroy(wtsn_sensor_manager *m) {
    free(m);
}

size_t wtsn_sensor_manager_count(wtsn_sensor_manager *m) {
    return m ? m->count : 0;
}

void wtsn_sensor_manager_for_each(wtsn_sensor_manager *m,
                                  void (*cb)(const wtsn_sensor *s, void *ud), void *ud) {
    if (!m || !cb) return;
    for (size_t i = 0; i < m->count; i++) cb(&m->sensors[i], ud);
}

wtsn_error wtsn_sensor_manager_upsert(wtsn_sensor_manager *m, const wtsn_sensor *s) {
    if (!m || !s) return WTSN_ERR_INVALID_ARG;
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->sensors[i].device_id, s->device_id) == 0 &&
            strcmp(m->sensors[i].sensor_id, s->sensor_id) == 0) {
            m->sensors[i] = *s;
            wtsn_db_sensor_upsert(m->db, s);
            wtsn_model_notify(&m->model, "changed");
            return WTSN_OK;
        }
    }
    if (m->count >= MAX_SENSORS) return WTSN_ERR_BUSY;
    m->sensors[m->count++] = *s;
    wtsn_db_sensor_upsert(m->db, s);
    wtsn_model_notify(&m->model, "changed");
    return WTSN_OK;
}

wtsn_error wtsn_sensor_manager_remove(wtsn_sensor_manager *m, const char *dev, const char *sid) {
    if (!m || !dev || !sid) return WTSN_ERR_INVALID_ARG;
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->sensors[i].device_id, dev) == 0 &&
            strcmp(m->sensors[i].sensor_id, sid) == 0) {
            memmove(&m->sensors[i], &m->sensors[i+1], (m->count - i - 1) * sizeof(wtsn_sensor));
            m->count--;
            wtsn_db_sensor_delete(m->db, dev, sid);
            wtsn_model_notify(&m->model, "changed");
            return WTSN_OK;
        }
    }
    return WTSN_ERR_NOT_FOUND;
}

static void make_sensor(const char *device_id, const char *sid, wtsn_sensor_type type,
                        const char *name, const char *unit, wtsn_sensor *out) {
    memset(out, 0, sizeof(*out));
    wtsn_strlcpy(out->device_id, device_id, sizeof(out->device_id));
    wtsn_strlcpy(out->sensor_id, sid, sizeof(out->sensor_id));
    out->type = type;
    wtsn_strlcpy(out->name, name, sizeof(out->name));
    wtsn_strlcpy(out->unit, unit, sizeof(out->unit));
    out->value = 0.0;
    out->healthy = true;
    out->last_update = time(NULL);
}

wtsn_error wtsn_sensor_manager_auto_detect(wtsn_sensor_manager *m, const char *device_id) {
    if (!m || !device_id) return WTSN_ERR_INVALID_ARG;
    wtsn_sensor s;
    make_sensor(device_id, "temp1", WTSN_SENSOR_TEMPERATURE, "Ambient Temperature", "degC", &s);
    wtsn_sensor_manager_upsert(m, &s);
    make_sensor(device_id, "press1", WTSN_SENSOR_PRESSURE, "Ambient Pressure", "hPa", &s);
    wtsn_sensor_manager_upsert(m, &s);
    make_sensor(device_id, "imu1", WTSN_SENSOR_IMU, "6-axis IMU", "g", &s);
    wtsn_sensor_manager_upsert(m, &s);
    make_sensor(device_id, "dist1", WTSN_SENSOR_DISTANCE, "Ultrasonic Distance", "cm", &s);
    wtsn_sensor_manager_upsert(m, &s);
    make_sensor(device_id, "gpio1", WTSN_SENSOR_GPIO, "GPIO Input", "", &s);
    wtsn_sensor_manager_upsert(m, &s);
    wtsn_log(WTSN_LOG_INFO, "auto-detected sensors for %s", device_id);
    return WTSN_OK;
}
