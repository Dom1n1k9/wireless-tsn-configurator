#include "wtsn_tsn.h"

#include "esp_log.h"

#include <string.h>

#include "wtsn_ptp.h"

static const char *TAG = "tsn";

static wtsn_tsn_state g_state;

wtsn_tsn_state *wtsn_tsn_get_state(void) { return &g_state; }

int wtsn_tsn_apply_qos(int priority, int traffic_class, int bw_kbps, int lat_ms, int preemption) {
    (void)bw_kbps; (void)lat_ms;
    g_state.priority = priority;
    g_state.traffic_class = traffic_class;
    g_state.preemption = preemption;
    ESP_LOGI(TAG, "QoS prio=%d tc=%d preempt=%d", priority, traffic_class, preemption);
    return 0;
}

int wtsn_tsn_apply_vlan(int vlan_id, const char *group) {
    g_state.vlan_id = vlan_id;
    ESP_LOGI(TAG, "VLAN %d group %s", vlan_id, group ? group : "");
    return 0;
}

int wtsn_tsn_apply_timesync(int mode, const char *gm) {
    g_state.timesync_mode = mode;
    ESP_LOGI(TAG, "time sync mode=%d gm=%s", mode, gm ? gm : "");
    wtsn_ptp_apply(mode, gm);
    return 0;
}

int wtsn_tsn_apply_tas(int64_t cycle_ns, const int *gates, const int64_t *durations, int entries) {
    g_state.tas_cycle_ns = cycle_ns;
    ESP_LOGI(TAG, "TAS cycle %lld ns, %d entries", (long long)cycle_ns, (int)entries);
    (void)gates; (void)durations;
    return 0;
}

int wtsn_tsn_apply_preemption(int preemption, const char *emac_csv, const char *pmac_csv) {
    g_state.preemption = preemption;
    ESP_LOGI(TAG, "preemption=%d eMAC[%s] pMAC[%s]", preemption,
             emac_csv ? emac_csv : "", pmac_csv ? pmac_csv : "");
    return 0;
}

int wtsn_tsn_apply_snapshot(const wtsn_config_snapshot *cfg) {
    if (!cfg) return -1;
    wtsn_tsn_apply_qos(cfg->priority, cfg->traffic_class, cfg->bandwidth_kbps,
                        cfg->latency_ms, cfg->preemption);
    wtsn_tsn_apply_vlan(cfg->vlan_id, cfg->group);
    wtsn_tsn_apply_timesync(cfg->timesync_mode, cfg->grandmaster);
    wtsn_tsn_apply_tas(cfg->tas_cycle_ns, cfg->gates, cfg->durations, cfg->gcl_entries);
    wtsn_tsn_apply_preemption(cfg->preemption, "", "");
    return 0;
}
