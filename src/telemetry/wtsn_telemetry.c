#include "telemetry/wtsn_telemetry.h"

#include "device/device_manager.h"

#include "mqtt/mqtt_client.h"

#include "common/log.h"
#include "common/str_util.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    TL_NONE = 0,
    TL_HEARTBEAT,
    TL_SYNC_REPORT,
    TL_SENSOR
} tl_kind;

struct wtsn_telemetry {
    wtsn_device_manager *devices;
    wtsn_timesync_manager *timesync;
    wtsn_trace *trace;
    wtsn_mqtt_client *mqtt;
};

/* Parse a JSON-ish key:value list like
 *   {"id":"esp-1","offset_ns":-120,"jitter_ns":40,"loss":0}
 * into a target string (the device id for status topics). Returns the kind. */
static tl_kind parse_payload(const char *payload, char *id_out, size_t id_cap,
                           int64_t *offset, int64_t *jitter, int *loss) {
    if (!payload) return TL_NONE;
    char buf[512];
    wtsn_strlcpy(buf, payload, sizeof(buf));
    id_out[0] = '\0';

    char *p = buf;
    tl_kind kind = TL_NONE;
    while (*p) {
        if (*p == '{' || *p == '}' || *p == '"') { p++; continue; }
        if (strncmp(p, "offset_ns", 9) == 0) {
            p = strchr(p, ':');
            if (!p) break;
            p++;
            *offset = strtoll(p, NULL, 10);
            kind = TL_SYNC_REPORT;
        } else if (strncmp(p, "jitter_ns", 10) == 0) {
            p = strchr(p, ':');
            if (!p) break;
            p++;
            *jitter = strtoll(p, NULL, 10);
        } else if (strncmp(p, "loss", 4) == 0) {
            p = strchr(p, ':');
            if (!p) break;
            p++;
            *loss = atoi(p);
        } else if (strncmp(p, "heartbeat", 9) == 0) {
            kind = TL_HEARTBEAT;
        } else if (strncmp(p, "id", 2) == 0) {
            p = strchr(p, ':');
            if (!p) break;
            p++;
            while (*p == ' ') p++;
            size_t cap = id_cap - 1, i = 0;
            while (i < cap && *p && *p != ',' && *p != '}') id_out[i++] = *p++;
            id_out[i] = '\0';
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    return kind;
}

static void on_message(const char *topic, const char *payload, size_t len, void *ud) {
    wtsn_telemetry *t = (wtsn_telemetry *)ud;
    (void)len;
    if (!topic) return;
    if (strcmp(topic, "tsn/status") == 0 || strncmp(topic, "tsn/status/", 12) == 0) {
        char id[WTSN_MAX_STR];
        int64_t off = 0, jit = 0;
        int loss = 0;
        tl_kind k = parse_payload(payload, id, sizeof(id), &off, &jit, &loss);
        /* device id: use the topic suffix if present, else the JSON "id" field */
        char node[WTSN_MAX_STR];
        node[0] = '\0';
        if (strncmp(topic, "tsn/status/", 12) == 0) {
            const char *base = topic + 11;
            size_t nl = strcspn(base, "/");
            if (nl >= sizeof(node)) nl = sizeof(node) - 1;
            memcpy(node, base, nl); node[nl] = '\0';
        }
        const char *dev = node[0] ? node : id;
        if (dev[0]) {
            if (k == TL_SYNC_REPORT && t->timesync) {
                wtsn_timesync_manager_record_report(t->timesync, dev, off, jit, 0, loss);
            }
            /* every status/telemetry message is a heartbeat for the node */
            if (t->devices) wtsn_device_manager_record_heartbeat(t->devices, dev);
        }
    }
}

wtsn_telemetry *wtsn_telemetry_create(wtsn_device_manager *devices,
                                      wtsn_timesync_manager *timesync,
                                      wtsn_trace *trace) {
    wtsn_telemetry *t = calloc(1, sizeof(wtsn_telemetry));
    if (!t) return NULL;
    t->devices = devices;
    t->timesync = timesync;
    t->trace = trace;
    return t;
}

void wtsn_telemetry_destroy(wtsn_telemetry *t) {
    if (t) wtsn_telemetry_detach(t);
    free(t);
}

wtsn_error wtsn_telemetry_attach(wtsn_telemetry *t, wtsn_mqtt_client *mqtt) {
    if (!t || !mqtt) return WTSN_ERR_INVALID_ARG;
    t->mqtt = mqtt;
    wtsn_mqtt_client_set_message_cb(mqtt, on_message, t);
    wtsn_mqtt_client_subscribe(mqtt, "tsn/status/#");
    wtsn_mqtt_client_subscribe(mqtt, "tsn/telemetry/#");
    return WTSN_OK;
}

void wtsn_telemetry_detach(wtsn_telemetry *t) {
    if (t && t->mqtt) wtsn_mqtt_client_set_message_cb(t->mqtt, NULL, NULL);
}
