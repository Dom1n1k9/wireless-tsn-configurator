#include "gateway/gateway.h"

#include "common/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char mqtt_topic[WTSN_MAX_STR];
    char opcua_path[WTSN_MAX_STR];
} mapping;

struct wtsn_gateway {
    wtsn_mqtt_client *mqtt;
    wtsn_opcua_server *opcua;
    mapping *maps;
    size_t map_count;
    size_t map_cap;
};

static void route_message(const char *topic, const char *payload, size_t len, void *ud) {
    wtsn_gateway *gw = (wtsn_gateway *)ud;
    (void)payload;
    (void)len;
    if (!topic) return;
    for (size_t i = 0; i < gw->map_count; i++) {
        if (strcmp(topic, gw->maps[i].mqtt_topic) == 0 ||
            strcmp(gw->maps[i].mqtt_topic, WTSN_GATEWAY_TOPIC_ALL) == 0) {
            wtsn_log(WTSN_LOG_INFO, "gateway route %s -> %s", topic, gw->maps[i].opcua_path);
        }
    }
}

wtsn_gateway *wtsn_gateway_create(wtsn_mqtt_client *mqtt, wtsn_opcua_server *opcua) {
    if (!mqtt || !opcua) return NULL;
    wtsn_gateway *gw = calloc(1, sizeof(wtsn_gateway));
    if (!gw) return NULL;
    gw->mqtt = mqtt;
    gw->opcua = opcua;
    gw->map_cap = 32;
    gw->maps = calloc(gw->map_cap, sizeof(mapping));
    if (!gw->maps) { free(gw); return NULL; }
    return gw;
}

void wtsn_gateway_destroy(wtsn_gateway *gw) {
    if (!gw) return;
    free(gw->maps);
    free(gw);
}

wtsn_error wtsn_gateway_map_topic(wtsn_gateway *gw, const char *mqtt_topic, const char *opcua_path) {
    if (!gw || !mqtt_topic || !opcua_path) return WTSN_ERR_INVALID_ARG;
    if (gw->map_count >= gw->map_cap) {
        size_t ncap = gw->map_cap * 2;
        mapping *n = realloc(gw->maps, ncap * sizeof(mapping));
        if (!n) return WTSN_ERR_NO_MEMORY;
        gw->maps = n;
        gw->map_cap = ncap;
    }
    mapping *m = &gw->maps[gw->map_count++];
    wtsn_strlcpy(m->mqtt_topic, mqtt_topic, sizeof(m->mqtt_topic));
    wtsn_strlcpy(m->opcua_path, opcua_path, sizeof(m->opcua_path));
    return WTSN_OK;
}

wtsn_error wtsn_gateway_start(wtsn_gateway *gw) {
    if (!gw) return WTSN_ERR_INVALID_ARG;
    wtsn_mqtt_client_set_message_cb(gw->mqtt, route_message, gw);
    for (size_t i = 0; i < gw->map_count; i++) {
        wtsn_mqtt_client_subscribe(gw->mqtt, gw->maps[i].mqtt_topic);
    }
    wtsn_log(WTSN_LOG_INFO, "gateway started with %zu mappings", gw->map_count);
    return WTSN_OK;
}
