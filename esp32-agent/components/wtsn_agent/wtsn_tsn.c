#include "wtsn_tsn.h"

#include "esp_log.h"

#include <string.h>

#include "wtsn_ptp.h"
#include "wtsn_cfg.h"

static const char *TAG = "tsn";

static wtsn_tsn_state g_state;
static bool g_loaded = false;

static void load_persisted(void) {
    if (g_loaded) return;
    g_loaded = true;
    int prio = 0, tc = 0, vlan = 0, pre = 0, tmode = 0;
    int64_t cycle = 0;
    int gates[WTSN_GCL_MAX];
    int64_t durs[WTSN_GCL_MAX];
    int entries = 0;
    if (wtsn_cfg_load_tsn_state(&prio, &tc, &vlan, &pre, &tmode, &cycle,
                                gates, durs, &entries)) {
        g_state.priority = prio;
        g_state.traffic_class = tc;
        g_state.vlan_id = vlan;
        g_state.preemption = pre;
        g_state.timesync_mode = tmode;
        g_state.tas_cycle_ns = cycle;
        g_state.gcl_entries = entries;
        for (int i = 0; i < entries; i++) {
            g_state.gates[i] = gates[i];
            g_state.durations[i] = durs[i];
        }
        ESP_LOGI(TAG, "restored from NVS: prio=%d vlan=%d pre=%d tas_cycle=%lld entries=%d",
                 prio, vlan, pre, (long long)cycle, entries);
        wtsn_ptp_apply(tmode, entries ? "persisted" : "restored");
    }
}

wtsn_tsn_state *wtsn_tsn_get_state(void) { load_persisted(); return &g_state; }

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
    if (entries > WTSN_GCL_MAX) entries = WTSN_GCL_MAX;
    if (entries < 0) entries = 0;
    g_state.tas_cycle_ns = cycle_ns;
    g_state.gcl_entries = entries;
    memset(g_state.gates, 0, sizeof(g_state.gates));
    memset(g_state.durations, 0, sizeof(g_state.durations));
    for (int i = 0; i < entries; i++) {
        g_state.gates[i] = gates ? gates[i] : 0;
        g_state.durations[i] = durations ? durations[i] : 0;
    }
    ESP_LOGI(TAG, "TAS cycle %lld ns, %d entries", (long long)cycle_ns, (int)entries);
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
    wtsn_cfg_save_tsn_state(g_state.priority, g_state.traffic_class, g_state.vlan_id,
                            g_state.preemption, g_state.timesync_mode, g_state.tas_cycle_ns,
                            g_state.gates, g_state.durations, g_state.gcl_entries);
    return 0;
}

/* Apply the persisted state back onto hardware on startup. Since the ESP32 has
 * no raw 802.3 egress, this re-applies what the firmware reports and leaves the
 * PTP control plane running; the state lives in our own NVS store (a 
 * hardware/config abstraction per device until a TSN-capable NIC is available). */
void wtsn_tsn_restore(void) {
    load_persisted();
    wtsn_ptp_apply(g_state.timesync_mode,
                   g_state.timesync_mode ? "persisted" : NULL);
    ESP_LOGI(TAG, "restored TSN state: prio=%d vlan=%d pre=%d cycle=%lld entries=%d",
             g_state.priority, g_state.vlan_id, g_state.preemption,
             (long long)g_state.tas_cycle_ns, g_state.gcl_entries);
}

/* Forget the persisted TSN configuration (used by the MQTT "reset" command so the
 * GUI's delete-device flow actually clears the node's assignment). */
void wtsn_tsn_reset_state(void) {
    g_loaded = true;   /* prevent a stale reload on the next get_state() call */
    g_state.priority = 0;
    g_state.traffic_class = 0;
    g_state.vlan_id = 0;
    g_state.preemption = 0;
    g_state.timesync_mode = 0;
    g_state.tas_cycle_ns = 0;
    g_state.gcl_entries = 0;
    memset(g_state.gates, 0, sizeof(g_state.gates));
    memset(g_state.durations, 0, sizeof(g_state.durations));
    wtsn_cfg_save_tsn_state(0, 0, 0, 0, 0, 0, NULL, NULL, 0);
    ESP_LOGI(TAG, "TSN state reset");
}

#define WTSN_STREAM_MAX 8

static wtsn_stream g_streams[WTSN_STREAM_MAX];
wtsn_stream *wtsn_tsn_streams = g_streams;
int wtsn_tsn_stream_count = 0;

wtsn_stream *wtsn_tsn_find_stream(const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < wtsn_tsn_stream_count; i++)
        if (strcmp(g_streams[i].stream_id, id) == 0) return &g_streams[i];
    return NULL;
}

int wtsn_tsn_apply_stream(const wtsn_stream *s) {
    if (!s) return -1;
    if (s->listener_count > WTSN_STREAM_MAX_LISTENERS) return -1;
    wtsn_stream *dst = wtsn_tsn_find_stream(s->stream_id);
    if (!dst) {
        if (wtsn_tsn_stream_count >= WTSN_STREAM_MAX) return -1;
        dst = &g_streams[wtsn_tsn_stream_count];
        wtsn_tsn_stream_count++;
    }
    memcpy(dst, s, sizeof(*s));
    for (int i = s->listener_count; i < WTSN_STREAM_MAX_LISTENERS; i++) dst->listeners[i][0] = '\0';
    dst->listener_count = s->listener_count;
    ESP_LOGI(TAG, "stream %s talker=%s listeners=%d vlan=%d", s->stream_id,
             s->talker, s->listener_count, s->vlan_id);
    return 0;
}
