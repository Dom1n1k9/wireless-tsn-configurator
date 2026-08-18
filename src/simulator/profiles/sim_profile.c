#include "simulator/profiles/sim_profile.h"

#include "simulator/common/sim_log.h"
#include "simulator/common/sim_str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void set_tsn_features(sim_device *d, const char *value) {
    d->tsn_feature_count = 0;
    char storage[512];
    char *parts[SIM_MAX_TSN_FEATURES];
    char copy[SIM_MAX_STR];
    sim_strlcpy(copy, value, sizeof(copy));
    int n = sim_strsplit(copy, ':', parts, SIM_MAX_TSN_FEATURES, storage, sizeof(storage));
    for (int i = 0; i < n && d->tsn_feature_count < SIM_MAX_TSN_FEATURES; i++) {
        sim_strlcpy(d->tsn_features[d->tsn_feature_count++], parts[i], 32);
    }
}

static void set_sensor(sim_device *d, const char *value, int index) {
    if (!d || !value || index >= SIM_MAX_SENSORS) return;
    if (d->sensor_count <= index) d->sensor_count = index + 1;
    sim_sensor *s = &d->sensors[index];
    char storage[256];
    char *parts[8];
    char copy[SIM_MAX_STR];
    sim_strlcpy(copy, value, sizeof(copy));
    int n = sim_strsplit(copy, ':', parts, 8, storage, sizeof(storage));
    if (n < 3) return;
    s->type = sim_sensor_type_parse(parts[0]);
    sim_strlcpy(s->name, parts[0], sizeof(s->name));
    sim_strlcpy(s->sensor_id, parts[1], sizeof(s->sensor_id));
    sim_strlcpy(s->unit, parts[2], sizeof(s->unit));
    if (n > 3) s->value = atof(parts[3]);
}

typedef void (*setter)(sim_device *, const char *);

static void set_qos(sim_device *d, const char *v) { (void)d; (void)v; }

static void apply_value(sim_device *d, const char *section, const char *key, const char *value) {
    if (strcmp(section, "device") == 0) {
        if (strcmp(key, "id") == 0) sim_strlcpy(d->id, value, sizeof(d->id));
        else if (strcmp(key, "name") == 0) sim_strlcpy(d->name, value, sizeof(d->name));
        else if (strcmp(key, "kind") == 0) d->kind = sim_device_kind_parse(value);
        else if (strcmp(key, "firmware") == 0) sim_strlcpy(d->firmware, value, sizeof(d->firmware));
        else if (strcmp(key, "model") == 0) sim_strlcpy(d->model, value, sizeof(d->model));
    } else if (strcmp(section, "network") == 0) {
        if (strcmp(key, "ip") == 0) sim_strlcpy(d->ip, value, sizeof(d->ip));
        else if (strcmp(key, "mac") == 0) sim_strlcpy(d->mac, value, sizeof(d->mac));
        else if (strcmp(key, "mqtt_topic") == 0) sim_strlcpy(d->mqtt_topic, value, sizeof(d->mqtt_topic));
    } else if (strcmp(section, "capabilities") == 0) {
        if (strcmp(key, "tsn_features") == 0) set_tsn_features(d, value);
    } else if (strcmp(section, "services") == 0) {
        if (strcmp(key, "qos") == 0) d->services_qos = (strcmp(value, "on") == 0);
        else if (strcmp(key, "vlan") == 0) d->services_vlan = (strcmp(value, "on") == 0);
        else if (strcmp(key, "timesync") == 0) d->services_timesync = (strcmp(value, "on") == 0);
        else if (strcmp(key, "tas") == 0) d->services_tas = (strcmp(value, "on") == 0);
        else if (strcmp(key, "sensors") == 0) d->services_sensors = (strcmp(value, "on") == 0);
    } else if (strcmp(section, "qos") == 0) {
        if (strcmp(key, "priority") == 0) d->qos_priority = atoi(value);
        else if (strcmp(key, "traffic_class") == 0) d->qos_traffic_class = atoi(value);
        else if (strcmp(key, "bandwidth_kbps") == 0) d->qos_bandwidth_kbps = atoi(value);
        else if (strcmp(key, "latency_ms") == 0) d->qos_latency_ms = atoi(value);
    } else if (strcmp(section, "vlan") == 0) {
        if (strcmp(key, "vlan_id") == 0) d->vlan_id = atoi(value);
        else if (strcmp(key, "group") == 0) sim_strlcpy(d->vlan_group, value, sizeof(d->vlan_group));
    } else if (strcmp(section, "timesync") == 0) {
        if (strcmp(key, "mode") == 0) d->timesync_mode = (strcmp(value, "master") == 0) ? 1 : 0;
        else if (strcmp(key, "grandmaster") == 0) sim_strlcpy(d->timesync_grandmaster, value, sizeof(d->timesync_grandmaster));
        else if (strcmp(key, "protocol") == 0) sim_strlcpy(d->timesync_protocol, value, sizeof(d->timesync_protocol));
        else if (strcmp(key, "offset_ns") == 0) d->timesync_offset_ns = atoll(value);
    } else if (strcmp(section, "tas") == 0) {
        if (strcmp(key, "cycle_time_ns") == 0) d->cycle_time_ns = atoll(value);
        else if (strcmp(key, "deploy_target") == 0) sim_strlcpy(d->gcl_deploy_target, value, sizeof(d->gcl_deploy_target));
    }
}

sim_error sim_profile_load(const char *path, sim_device *out) {
    if (!path || !out) return SIM_ERR_INVALID_ARG;
    FILE *f = fopen(path, "r");
    if (!f) return SIM_ERR_IO;

    sim_error e = sim_profile_default(out);
    (void)e;

    char line[512];
    char section[SIM_MAX_STR] = "";
    while (fgets(line, sizeof(line), f)) {
        sim_str_trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) { fclose(f); return SIM_ERR_CONFIG; }
            *end = '\0';
            sim_strlcpy(section, line + 1, sizeof(section));
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char key[SIM_MAX_STR];
        sim_str_trim(line);
        sim_strlcpy(key, line, sizeof(key));
        char *val = eq + 1;
        sim_str_trim(val);

        if (strcmp(section, "gcl") == 0) {
            if (out->gcl_count < SIM_MAX_GCL_ENTRIES) {
                char storage[64];
                char *parts[4];
                char copy[SIM_MAX_STR];
                sim_strlcpy(copy, val, sizeof(copy));
                int n = sim_strsplit(copy, ':', parts, 4, storage, sizeof(storage));
                if (n >= 2) {
                    out->gcl[out->gcl_count].gate_state =
                        (strcmp(parts[0], "open") == 0) ? 0x01 : 0x00;
                    out->gcl[out->gcl_count].duration_ns = atoll(parts[1]);
                    out->gcl_count++;
                }
            }
        } else if (strcmp(section, "sensors") == 0) {
            set_sensor(out, val, atoi(key));
        } else {
            apply_value(out, section, key, val);
        }
    }
    fclose(f);

    if (strlen(out->id) == 0) {
        sim_log(SIM_LOG_ERROR, "profile %s missing device id", path);
        return SIM_ERR_CONFIG;
    }
    return SIM_OK;
}

sim_error sim_profile_default(sim_device *out) {
    if (!out) return SIM_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->kind = SIM_DEVICE_KIND_GENERIC;
    out->status = SIM_DEVICE_ONLINE;
    memset(out->id, 0, sizeof(out->id));
    return SIM_OK;
}
