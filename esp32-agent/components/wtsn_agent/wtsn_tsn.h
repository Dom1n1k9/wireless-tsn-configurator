#ifndef WTSN_TSN_H
#define WTSN_TSN_H

#include <stdint.h>
#include <stdbool.h>

#define WTSN_GCL_MAX 32

/* Full configuration snapshot received on tsn/cmd/<id>/apply */
typedef struct {
    int priority;      /* 0-7 */
    int traffic_class;
    int bandwidth_kbps;
    int latency_ms;
    int preemption;    /* 0 = off, 1 = on */
    int vlan_id;      /* 0 = none */
    char group[32];
    int timesync_mode; /* 0 disabled,1 local GM,2 external GM,3 auto */
    char grandmaster[32];
    int64_t tas_cycle_ns;
    int gcl_entries;
    int gates[WTSN_GCL_MAX];        /* per entry gate_state */
    int64_t durations[WTSN_GCL_MAX];
} wtsn_config_snapshot;

/* current applied state, kept for status reporting */
typedef struct {
    int priority;
    int traffic_class;
    int preemption;
    int vlan_id;
    int timesync_mode;
    int64_t tas_cycle_ns;
} wtsn_tsn_state;

wtsn_tsn_state *wtsn_tsn_get_state(void);

/* apply a full snapshot, return 0 on success (used by /apply handler) */
int wtsn_tsn_apply_snapshot(const wtsn_config_snapshot *cfg);

int wtsn_tsn_apply_qos(int priority, int traffic_class, int bw_kbps, int lat_ms, int preemption);
int wtsn_tsn_apply_vlan(int vlan_id, const char *group);
int wtsn_tsn_apply_timesync(int mode, const char *gm);
int wtsn_tsn_apply_tas(int64_t cycle_ns, const int *gates, const int64_t *durations, int entries);
int wtsn_tsn_apply_preemption(int preemption, const char *emac_csv, const char *pmac_csv);

#endif
