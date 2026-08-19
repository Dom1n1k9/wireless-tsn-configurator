#include "gateway/gateway_pubsub.h"

#include "common/log.h"
#include "common/str_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char mqtt_topic[WTSN_PUBSUB_TOPIC_MAX];
    char pubsub_dataset[WTSN_PUBSUB_DATASET_NAME_MAX];
} gpmap;

struct wtsn_gateway_pubsub {
    wtsn_mqtt_client *mqtt;
    wtsn_pubsub *pubsub;
    wtsn_trace *trace;
    gpmap *maps;
    size_t map_count;
    size_t map_cap;
    bool started;
};

static void on_mqtt(const char *topic, const char *payload, size_t len, void *ud) {
    wtsn_gateway_pubsub *gw = (wtsn_gateway_pubsub *)ud;
    if (!gw || !topic) return;
    for (size_t i = 0; i < gw->map_count; i++) {
        if (strcmp(topic, gw->maps[i].mqtt_topic) == 0) {
            wtsn_pubsub_dataset ds;
            memset(&ds, 0, sizeof(ds));
            wtsn_strlcpy(ds.name, gw->maps[i].pubsub_dataset, sizeof(ds.name));
            wtsn_strlcpy(ds.topic, topic, sizeof(ds.topic));
            /* represent inbound payload as a single double field for the pubsub side */
            wtsn_pubsub_field_set_int32(&ds.fields[0], "mqtt_payload_len", (int32_t)len);
            ds.field_count = 1;
            wtsn_pubsub_publish(gw->pubsub, &ds);
            if (gw->trace) {
                wtsn_trace_add_comm(gw->trace, "gw", "MQTT->PubSub");
                wtsn_trace_add_frame(gw->trace, "gw", (const unsigned char *)payload, len);
            }
        }
    }
}

static void on_pubsub(const wtsn_pubsub_dataset *ds, void *ud) {
    wtsn_gateway_pubsub *gw = (wtsn_gateway_pubsub *)ud;
    if (!gw || !ds) return;
    for (size_t i = 0; i < gw->map_count; i++) {
        if (strcmp(ds->name, gw->maps[i].pubsub_dataset) == 0 ||
            strcmp(ds->topic, gw->maps[i].mqtt_topic) == 0) {
            char payload[WTSN_PUBSUB_TOPIC_MAX + 32];
            snprintf(payload, sizeof(payload), "pubsub:%s:%zu_f", ds->topic, ds->field_count);
            wtsn_mqtt_client_publish(gw->mqtt, gw->maps[i].mqtt_topic, payload);
            if (gw->trace) {
                wtsn_trace_add_comm(gw->trace, "gw", "PubSub->MQTT");
                wtsn_trace_add_config(gw->trace, "gw", ds->name);
            }
        }
    }
}

wtsn_gateway_pubsub *wtsn_gateway_pubsub_create(wtsn_mqtt_client *mqtt, wtsn_pubsub *pubsub, wtsn_trace *trace) {
    if (!mqtt || !pubsub) return NULL;
    wtsn_gateway_pubsub *gw = calloc(1, sizeof(wtsn_gateway_pubsub));
    if (!gw) return NULL;
    gw->mqtt = mqtt;
    gw->pubsub = pubsub;
    gw->trace = trace;
    gw->map_cap = 16;
    gw->maps = calloc(gw->map_cap, sizeof(gpmap));
    if (!gw->maps) { free(gw); return NULL; }
    return gw;
}

void wtsn_gateway_pubsub_destroy(wtsn_gateway_pubsub *gw) {
    if (!gw) return;
    free(gw->maps);
    free(gw);
}

wtsn_error wtsn_gateway_pubsub_map_topic(wtsn_gateway_pubsub *gw, const char *mqtt_topic, const char *pubsub_dataset) {
    if (!gw || !mqtt_topic || !pubsub_dataset || gw->map_count >= gw->map_cap)
        return WTSN_ERR_INVALID_ARG;
    gpmap *m = &gw->maps[gw->map_count++];
    wtsn_strlcpy(m->mqtt_topic, mqtt_topic, sizeof(m->mqtt_topic));
    wtsn_strlcpy(m->pubsub_dataset, pubsub_dataset, sizeof(m->pubsub_dataset));
    return WTSN_OK;
}

wtsn_error wtsn_gateway_pubsub_start(wtsn_gateway_pubsub *gw) {
    if (!gw || gw->started) return WTSN_ERR_INVALID_ARG;
    wtsn_mqtt_client_set_message_cb(gw->mqtt, on_mqtt, gw);
    wtsn_pubsub_loopback_tap_hook(on_pubsub, gw);
    for (size_t i = 0; i < gw->map_count; i++) {
        wtsn_mqtt_client_subscribe(gw->mqtt, gw->maps[i].mqtt_topic);
    }
    gw->started = true;
    wtsn_log(WTSN_LOG_INFO, "mqtt over pubsub gateway started: %zu mappings", gw->map_count);
    return WTSN_OK;
}
