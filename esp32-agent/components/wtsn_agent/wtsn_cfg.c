#include "wtsn_cfg.h"
#include "wtsn_tsn.h"

#include "nvs_flash.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

#define WTSN_NVS_NAMESPACE "wtsn"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_MQTT_HOST "mqtt_host"
#define KEY_MQTT_PORT "mqtt_port"
#define KEY_DEVICE_ID "device_id"

void wtsn_strlcpy(char *dst, const char *src, size_t sz) {
    if (!dst || sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= sz) n = sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool wtsn_nvs_get_str(const char *key, char *out, size_t sz, const char *dflt) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        wtsn_strlcpy(out, dflt, sz);
        return false;
    }
    size_t len = sz;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) { wtsn_strlcpy(out, dflt, sz); return false; }
    return true;
}

static int wtsn_nvs_get_int(const char *key, int dflt) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return dflt;
    int32_t v = dflt;
    esp_err_t err = nvs_get_i32(h, key, &v);
    nvs_close(h);
    return err == ESP_OK ? (int)v : dflt;
}

static void wtsn_nvs_set_str(const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static void wtsn_nvs_set_int(const char *key, int val) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

void wtsn_cfg_set_device_id(const char *device_id) {
    if (device_id) wtsn_nvs_set_str(KEY_DEVICE_ID, device_id);
}

bool wtsn_cfg_load_device_id(char *out, size_t *sz) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_str(h, KEY_DEVICE_ID, out, sz);
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

bool wtsn_cfg_load(char *device_id, size_t device_id_sz,
                   char *wifi_ssid, size_t ssid_sz,
                   char *wifi_pass, size_t pass_sz,
                   char *mqtt_host, size_t host_sz,
                   int *mqtt_port) {
    if (device_id && device_id_sz) wtsn_strlcpy(device_id, "", device_id_sz);
    nvs_handle_t h = 0;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_close(h);
    if (device_id && device_id_sz) {
        char tmp[32] = {0};
        if (wtsn_nvs_get_str(KEY_DEVICE_ID, tmp, sizeof(tmp), "")) {
            wtsn_strlcpy(device_id, tmp, device_id_sz);
        }
    }
    wtsn_strlcpy(wifi_ssid, "", ssid_sz);
    wtsn_nvs_get_str(KEY_WIFI_SSID, wifi_ssid, ssid_sz, "");
    wtsn_nvs_get_str(KEY_WIFI_PASS, wifi_pass, pass_sz, "");
    wtsn_strlcpy(mqtt_host, "", host_sz);
    if (!wtsn_nvs_get_str(KEY_MQTT_HOST, mqtt_host, host_sz, "")
        || mqtt_host[0] == '\0') wtsn_strlcpy(mqtt_host, "wtsn-broker.local", host_sz);
    *mqtt_port = wtsn_nvs_get_int(KEY_MQTT_PORT, 1883);
    return true;
}

bool wtsn_cfg_save(const char *device_id, const char *wifi_ssid,
                   const char *wifi_pass, const char *mqtt_host, int mqtt_port) {
    if (device_id) wtsn_nvs_set_str(KEY_DEVICE_ID, device_id);
    if (wifi_ssid) wtsn_nvs_set_str(KEY_WIFI_SSID, wifi_ssid);
    if (wifi_pass) wtsn_nvs_set_str(KEY_WIFI_PASS, wifi_pass);
    if (mqtt_host) wtsn_nvs_set_str(KEY_MQTT_HOST, mqtt_host);
    wtsn_nvs_set_int(KEY_MQTT_PORT, mqtt_port);
    return true;
}

void wtsn_cfg_set_wifi(const char *ssid, const char *pass) {
    if (ssid) wtsn_nvs_set_str(KEY_WIFI_SSID, ssid);
    if (pass) wtsn_nvs_set_str(KEY_WIFI_PASS, pass);
}

#define KEY_TSN_PRIO      "tsn_prio"
#define KEY_TSN_TC        "tsn_tc"
#define KEY_TSN_VLAN      "tsn_vlan"
#define KEY_TSN_PRE       "tsn_pre"
#define KEY_TSN_TMODE     "tsn_tmode"
#define KEY_TSN_CYCLE     "tsn_cycle"
#define KEY_TSN_GCL_CNT   "tsn_gcl_cnt"
#define KEY_TSN_GATES     "tsn_gates"
#define KEY_TSN_DURS      "tsn_durs"

static void wtsn_nvs_set_blob(const char *key, const void *val, size_t len) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, val, len);
    nvs_commit(h);
    nvs_close(h);
}

static size_t wtsn_nvs_get_blob(const char *key, void *out, size_t len) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t got = len;
    esp_err_t err = nvs_get_blob(h, key, out, &got);
    nvs_close(h);
    return (err == ESP_OK) ? got : 0;
}

bool wtsn_cfg_save_tsn_state(const int priority, const int traffic_class,
                             const int vlan_id, const int preemption,
                             const int timesync_mode, const int64_t tas_cycle_ns,
                             const int *gates, const int64_t *durations,
                             const int gcl_entries) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_i32(h, KEY_TSN_PRIO, priority);
    nvs_set_i32(h, KEY_TSN_TC, traffic_class);
    nvs_set_i32(h, KEY_TSN_VLAN, vlan_id);
    nvs_set_i32(h, KEY_TSN_PRE, preemption);
    nvs_set_i32(h, KEY_TSN_TMODE, timesync_mode);
    nvs_set_i64(h, KEY_TSN_CYCLE, tas_cycle_ns);
    nvs_set_i32(h, KEY_TSN_GCL_CNT, gcl_entries);
    if (gcl_entries > 0 && gates && durations) {
        int g[WTSN_GCL_MAX];
        int64_t d[WTSN_GCL_MAX];
        for (int i = 0; i < WTSN_GCL_MAX; i++) {
            g[i] = (i < gcl_entries) ? gates[i] : 0;
            d[i] = (i < gcl_entries) ? durations[i] : 0;
        }
        nvs_set_blob(h, KEY_TSN_GATES, g, sizeof(g));
        nvs_set_blob(h, KEY_TSN_DURS, d, sizeof(d));
    }
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool wtsn_cfg_load_tsn_state(int *priority, int *traffic_class, int *vlan_id,
                             int *preemption, int *timesync_mode, int64_t *tas_cycle_ns,
                             int *gates, int64_t *durations, int *gcl_entries) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    int32_t iv;
    int64_t lv;
    int32_t cnt = 0;
    bool ok = true;
    ok &= (nvs_get_i32(h, KEY_TSN_PRIO, &iv) == ESP_OK);  if (ok) *priority = iv;
    if (nvs_get_i32(h, KEY_TSN_TC, &iv) == ESP_OK) *traffic_class = iv;
    if (nvs_get_i32(h, KEY_TSN_VLAN, &iv) == ESP_OK) *vlan_id = iv;
    if (nvs_get_i32(h, KEY_TSN_PRE, &iv) == ESP_OK) *preemption = iv;
    if (nvs_get_i32(h, KEY_TSN_TMODE, &iv) == ESP_OK) *timesync_mode = iv;
    if (nvs_get_i64(h, KEY_TSN_CYCLE, &lv) == ESP_OK) *tas_cycle_ns = lv;
    if (nvs_get_i32(h, KEY_TSN_GCL_CNT, &cnt) == ESP_OK && cnt > 0 && cnt <= WTSN_GCL_MAX) {
        int g[WTSN_GCL_MAX];
        int64_t d[WTSN_GCL_MAX];
        size_t gl = sizeof(g), dl = sizeof(d);
        if (nvs_get_blob(h, KEY_TSN_GATES, g, &gl) == ESP_OK &&
            nvs_get_blob(h, KEY_TSN_DURS, d, &dl) == ESP_OK) {
            for (int i = 0; i < cnt; i++) { gates[i] = g[i]; durations[i] = d[i]; }
            *gcl_entries = cnt;
        }
    }
    nvs_close(h);
    return ok;
}
