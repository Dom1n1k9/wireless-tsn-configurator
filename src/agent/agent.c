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

/* minimal root-level JSON extraction (no external JSON lib) */
static const char *json_val(const char *json, const char *key) {
    if (!json) return NULL;
    size_t klen = strlen(key);
    const char *p = json;
    while (p && *p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '{' || *p == ',')) p++;
        if (*p != '"') { while (*p && *p != '{' && *p != ',' && *p != '}') p++; continue; }
        const char *ks = p + 1;
        const char *ke = strchr(ks, '"');
        if (!ke) break;
        if ((size_t)(ke - ks) == klen && memcmp(ks, key, klen) == 0) {
            const char *colon = ke + 1;
            while (*colon && (*colon != ':')) colon++;
            if (!*colon) break;
            const char *v = colon + 1;
            while (*v && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
            return v;
        }
        /* advance past this value to the next key */
        p = ke + 1;
        while (*p && *p != ',' && *p != '{' && *p != '}') p++;
    }
    return NULL;
}

static int json_get_int(const char *json, const char *key, int *out) {
    const char *v = json_val(json, key);
    if (!v || *v == '"') return 0;
    char buf[16]; size_t i = 0;
    if (*v == '-') buf[i++] = *v++;
    while (*v >= '0' && *v <= '9' && i < sizeof(buf) - 1) buf[i++] = *v++;
    buf[i] = '\0';
    *out = atoi(buf);
    return i > 0;
}

static void json_get_str(const char *json, const char *key, char *out, size_t sz) {
    out[0] = '\0';
    const char *v = json_val(json, key);
    if (!v || *v != '"') return;
    const char *s = v + 1;
    const char *e = strchr(s, '"');
    if (!e) return;
    size_t n = (size_t)(e - s);
    if (n >= sz) n = sz - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

static void send_ack(wtsn_agent *a, bool ok) {
    if (!a->mqtt) return;
    char topic[64];
    snprintf(topic, sizeof(topic), "tsn/ack/%s", a->device_id);
    char payload[96];
    snprintf(payload, sizeof(payload), "{\"id\":\"%s\",\"ok\":%s}",
             a->device_id, ok ? "true" : "false");
    wtsn_mqtt_client_publish(a->mqtt, topic, payload);
}

/* Apply a full JSON snapshot (same schema as the ESP32 agent), so the
   host agent (Linux/RPi) speaks the same protocol as the embedded one. */
static wtsn_error apply_snapshot(wtsn_agent *a, const char *payload) {
    int p, t, bw, lat, pr, vid, tsmode;
    int have = 0;
    if (json_get_int(payload, "priority", &p)) { have = 1; }
    if (!json_get_int(payload, "traffic_class", &t)) t = 0;
    if (!json_get_int(payload, "bandwidth_kbps", &bw)) bw = 0;
    if (!json_get_int(payload, "latency_ms", &lat)) lat = 0;
    if (!json_get_int(payload, "preemption", &pr)) pr = 0;
    if (!json_get_int(payload, "vlan_id", &vid)) vid = 0;
    if (!json_get_int(payload, "timesync_mode", &tsmode)) tsmode = 0;

    char group[32] = "", gm[32] = "";
    json_get_str(payload, "group", group, sizeof(group));
    json_get_str(payload, "grandmaster", gm, sizeof(gm));

    if (have) a->ops.apply_qos(a->state, p, t, bw, lat, pr);
    if (vid > 0) a->ops.apply_vlan(a->state, vid, group);
    a->ops.apply_timesync(a->state, tsmode, gm[0] ? gm : NULL);

    int64_t cycle = 0;
    const char *cv = json_val(payload, "tas_cycle_ns");
    if (cv && *cv != '"') cycle = atoll(cv);
    a->ops.apply_tas(a->state, cycle, NULL, 0);
    return WTSN_OK;
}

/* Route inbound MQTT topic tsn/cmd/<id>/<command> to the right handler. */
static void on_message(const char *topic, const char *payload, size_t len, void *ud) {
    wtsn_agent *a = (wtsn_agent *)ud;
    if (!a) return;
    (void)len;
    if (strstr(topic, "/apply")) {
        if (payload[0] == '{') { apply_snapshot(a, payload); send_ack(a, true); }
        else send_ack(a, false);
        return;
    }
    const char *cmd = strrchr(topic, '/');
    cmd = cmd ? cmd + 1 : topic;
    wtsn_error e = wtsn_agent_handle_command(a, cmd, payload);
    wtsn_log(WTSN_LOG_INFO, "agent cmd %s -> %s", cmd, wtsn_error_str(e));
    send_ack(a, e == WTSN_OK);
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
        wtsn_mqtt_client_subscribe(a->mqtt, "tsn/fx/#");
        wtsn_mqtt_client_set_message_cb(a->mqtt, on_message, a);
        wtsn_mqtt_client_loop_start(a->mqtt);
        /* announce so the webgui discovers this node */
        char buff[64];
        snprintf(buff, sizeof(buff), "{\"id\":\"%s\"}", a->device_id);
        wtsn_mqtt_client_publish(a->mqtt, "tsn/discover", buff);
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
    } else if (strcmp(command, "fx") == 0) {
        /* FX over MQTT: publish a dataset into the C2C field flow */
        const char *group = token_at(payload, 0);
        if (!group[0]) group = "239.255.0.1";
        const char *dataset = token_at(payload, 1);
        if (!dataset[0]) dataset = "wtsnData";
        if (a->platform == AGENT_PLATFORM_LINUX ||
            a->platform == AGENT_PLATFORM_RASPBERRY_PI) {
            return agt_linux_send_fx_multicast(a->state, group,
                (const unsigned char *)dataset, strlen(dataset));
        }
        wtsn_log(WTSN_LOG_INFO, "[%s] fx multicast to group %s dataset %s",
                 a->platform_str, group, dataset);
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
