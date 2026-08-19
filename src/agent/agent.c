#include "agent/agent.h"
#include "agent/agent_providers.h"

#include "common/log.h"
#include "common/str_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *token_at(const char *payload, int index) {
    static char storage[128];
    static char *parts[8];
    char copy[WTSN_MAX_STR];
    wtsn_strlcpy(copy, payload ? payload : "", sizeof(copy));
    /* split on ':' and return index-th token */
    int nparts = 0;
    char *save = NULL;
    char *t = strtok_r(copy, ":", &save);
    while (t && nparts < 8) { parts[nparts++] = t; t = strtok_r(NULL, ":", &save); }
    if (index >= nparts) return "";
    return parts[index];
}

wtsn_agent *wtsn_agent_create(const char *device_id, const char *platform_str,
                              const char *mqtt_host, int mqtt_port) {
    if (!device_id || !platform_str) return NULL;
    wtsn_agent *a = calloc(1, sizeof(wtsn_agent));
    if (!a) return NULL;
    wtsn_strlcpy(a->device_id, device_id, sizeof(a->device_id));
    wtsn_strlcpy(a->platform_str, platform_str, sizeof(a->platform_str));
    a->platform = agt_platform_from_string(platform_str);
    a->mqtt_port = mqtt_port;
    wtsn_strlcpy(a->mqtt_host, mqtt_host ? mqtt_host : "localhost", sizeof(a->mqtt_host));

    /* pick provider */
    if (a->platform == AGENT_PLATFORM_LINUX || a->platform == AGENT_PLATFORM_RASPBERRY_PI) {
        a->ops = agt_linux_ops();
        a->state = agt_linux_state_create(NULL);
    } else {
        a->ops = agt_embedded_ops(a->platform);
        a->state = agt_embedded_state_create();
        agt_embedded_state_set(a->state, a->platform);
    }
    if (!a->state) { free(a); return NULL; }
    return a;
}

void wtsn_agent_destroy(wtsn_agent *a) {
    if (!a) return;
    if (a->ops.destroy) a->ops.destroy(a->state);
    if (a->mqtt) wtsn_mqtt_client_destroy(a->mqtt);
    free(a);
}

wtsn_error wtsn_agent_start(wtsn_agent *a) {
    if (!a) return WTSN_ERR_INVALID_ARG;
    if (a->ops.init) a->ops.init(a->state);
    if (strlen(a->mqtt_host) > 0) {
        a->mqtt = wtsn_mqtt_client_create(NULL);
        wtsn_mqtt_client_connect(a->mqtt, a->mqtt_host, a->mqtt_port,
                                 a->device_id, NULL, NULL);
        wtsn_mqtt_client_subscribe(a->mqtt, "tsn/cmd/#");
        wtsn_mqtt_client_loop_start(a->mqtt);
    }
    wtsn_log(WTSN_LOG_INFO, "agent %s started (platform=%s)", a->device_id, a->platform_str);
    return WTSN_OK;
}

wtsn_error wtsn_agent_handle_command(wtsn_agent *a, const char *command, const char *payload) {
    if (!a || !command) return WTSN_ERR_INVALID_ARG;

    if (strcmp(command, "qos") == 0) {
        int p = atoi(token_at(payload, 0));
        int t = atoi(token_at(payload, 1));
        int b = atoi(token_at(payload, 2));
        int l = atoi(token_at(payload, 3));
        int pr = atoi(token_at(payload, 4));
        return a->ops.apply_qos(a->state, p, t, b, l, pr);
    } else if (strcmp(command, "vlan") == 0) {
        int vid = atoi(token_at(payload, 0));
        return a->ops.apply_vlan(a->state, vid, token_at(payload, 1));
    } else if (strcmp(command, "timesync") == 0) {
        int mode = atoi(token_at(payload, 0));
        return a->ops.apply_timesync(a->state, mode, token_at(payload, 1));
    } else if (strcmp(command, "tas") == 0) {
        int64_t cycle = atoll(payload ? payload : "0");
        return a->ops.apply_tas(a->state, cycle, NULL, 0);
    } else if (strcmp(command, "status") == 0) {
        if (a->mqtt) wtsn_mqtt_client_publish(a->mqtt, "tsn/status", a->device_id);
        return WTSN_OK;
    }
    return WTSN_ERR_NOT_FOUND;
}

int wtsn_agent_run_loop(wtsn_agent *a) {
    (void)a;
    /* main loop; in this build the caller drives MQTT callbacks. */
    for (;;) {
        if (a && a->mqtt) wtsn_mqtt_client_loop_start(a->mqtt);
        return 0;
    }
    return 0;
}
