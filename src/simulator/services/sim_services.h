#ifndef SIM_SERVICES_H
#define SIM_SERVICES_H

#include "simulator/core/sim_device.h"

typedef enum {
    SIM_TSYNC_DISABLED = 0,
    SIM_TSYNC_SLAVE = 1,
    SIM_TSYNC_MASTER_GRANDMASTER = 2,
    SIM_TSYNC_EXTERNAL_GRANDMASTER = 3
} sim_timesync_mode;

const char *sim_timesync_mode_str(int mode);

/* QoS */
sim_error sim_qos_apply(sim_device *d, int priority, int traffic_class,
                        int bandwidth_kbps, int latency_ms);
sim_error sim_qos_validate(int priority, int traffic_class, int bandwidth_kbps, int latency_ms);

/* VLAN */
sim_error sim_vlan_apply(sim_device *d, int vlan_id, const char *group);
sim_error sim_vlan_validate(int vlan_id);

/* Time sync */
sim_error sim_timesync_apply(sim_device *d, int mode, const char *grandmaster,
                            const char *protocol, int64_t offset_ns);
sim_error sim_timesync_validate(int mode);

/* TAS */
sim_error sim_tas_apply(sim_device *d, int64_t cycle_time_ns, const char *deploy_target);
int sim_gcl_gate_open(const sim_device *d);

/* Sensors */
sim_error sim_sensor_add(sim_device *d, sim_sensor_type type, const char *id,
                        const char *name, const char *unit, double value);
void sim_sensor_report(const sim_device *d, char *out, size_t out_size);

#endif
