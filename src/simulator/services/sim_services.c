#include "simulator/services/sim_services.h"

#include "simulator/common/sim_str.h"

#include <stdio.h>
#include <string.h>

const char *sim_timesync_mode_str(int mode) {
    switch (mode) {
    case SIM_TSYNC_DISABLED: return "disabled";
    case SIM_TSYNC_SLAVE: return "slave";
    case SIM_TSYNC_MASTER_GRANDMASTER: return "master";
    case SIM_TSYNC_EXTERNAL_GRANDMASTER: return "external";
    default: return "unknown";
    }
}

sim_error sim_qos_validate(int priority, int traffic_class, int bandwidth_kbps, int latency_ms) {
    if (priority < 0 || priority > 7) return SIM_ERR_INVALID_ARG;
    if (traffic_class < 0 || traffic_class > 7) return SIM_ERR_INVALID_ARG;
    if (bandwidth_kbps < 0 || bandwidth_kbps > 10000000) return SIM_ERR_INVALID_ARG;
    if (latency_ms < 0 || latency_ms > 60000) return SIM_ERR_INVALID_ARG;
    return SIM_OK;
}

sim_error sim_qos_apply(sim_device *d, int priority, int traffic_class,
                        int bandwidth_kbps, int latency_ms) {
    if (!d) return SIM_ERR_INVALID_ARG;
    if (sim_qos_validate(priority, traffic_class, bandwidth_kbps, latency_ms) != SIM_OK)
        return SIM_ERR_INVALID_ARG;
    d->qos_priority = priority;
    d->qos_traffic_class = traffic_class;
    d->qos_bandwidth_kbps = bandwidth_kbps;
    d->qos_latency_ms = latency_ms;
    return SIM_OK;
}

sim_error sim_vlan_validate(int vlan_id) {
    if (vlan_id < 1 || vlan_id > 4094) return SIM_ERR_INVALID_ARG;
    return SIM_OK;
}

sim_error sim_vlan_apply(sim_device *d, int vlan_id, const char *group) {
    if (!d || !group) return SIM_ERR_INVALID_ARG;
    if (sim_vlan_validate(vlan_id) != SIM_OK) return SIM_ERR_INVALID_ARG;
    d->vlan_id = vlan_id;
    sim_strlcpy(d->vlan_group, group, sizeof(d->vlan_group));
    return SIM_OK;
}

sim_error sim_timesync_validate(int mode) {
    if (mode < SIM_TSYNC_DISABLED || mode > SIM_TSYNC_EXTERNAL_GRANDMASTER)
        return SIM_ERR_INVALID_ARG;
    return SIM_OK;
}

sim_error sim_timesync_apply(sim_device *d, int mode, const char *grandmaster,
                            const char *protocol, int64_t offset_ns) {
    if (!d || !grandmaster || !protocol) return SIM_ERR_INVALID_ARG;
    if (sim_timesync_validate(mode) != SIM_OK) return SIM_ERR_INVALID_ARG;
    d->timesync_mode = mode;
    sim_strlcpy(d->timesync_grandmaster, grandmaster, sizeof(d->timesync_grandmaster));
    sim_strlcpy(d->timesync_protocol, protocol, sizeof(d->timesync_protocol));
    d->timesync_offset_ns = offset_ns;
    return SIM_OK;
}

sim_error sim_tas_apply(sim_device *d, int64_t cycle_time_ns, const char *deploy_target) {
    if (!d || !deploy_target || cycle_time_ns <= 0) return SIM_ERR_INVALID_ARG;
    d->cycle_time_ns = cycle_time_ns;
    sim_strlcpy(d->gcl_deploy_target, deploy_target, sizeof(d->gcl_deploy_target));
    return SIM_OK;
}

int sim_gcl_gate_open(const sim_device *d) {
    if (!d) return 0;
    return d->gcl_state == 1;
}

sim_error sim_sensor_add(sim_device *d, sim_sensor_type type, const char *id,
                        const char *name, const char *unit, double value) {
    if (!d || !id || d->sensor_count >= SIM_MAX_SENSORS) return SIM_ERR_INVALID_ARG;
    sim_sensor *s = &d->sensors[d->sensor_count++];
    memset(s, 0, sizeof(*s));
    s->type = type;
    sim_strlcpy(s->sensor_id, id, sizeof(s->sensor_id));
    sim_strlcpy(s->name, name ? name : id, sizeof(s->name));
    sim_strlcpy(s->unit, unit ? unit : "", sizeof(s->unit));
    s->value = value;
    s->min = (type == SIM_SENSOR_TEMPERATURE) ? -40 : 0;
    s->max = (type == SIM_SENSOR_TEMPERATURE) ? 125 : 100000;
    s->step = (type == SIM_SENSOR_GPIO) ? 0 : (s->max - s->min) / 400.0;
    return SIM_OK;
}

void sim_sensor_report(const sim_device *d, char *out, size_t out_size) {
    if (!d || !out || out_size == 0) return;
    size_t off = 0;
    for (int i = 0; i < d->sensor_count && off < out_size; i++) {
        const sim_sensor *s = &d->sensors[i];
        int n = snprintf(out + off, out_size - off, "%s:%.1f%s%s",
                         s->sensor_id, s->value, s->unit[0] ? s->unit : "",
                         (i + 1 < d->sensor_count) ? ", " : "");
        if (n < 0) return;
        off += (size_t)n;
    }
}
