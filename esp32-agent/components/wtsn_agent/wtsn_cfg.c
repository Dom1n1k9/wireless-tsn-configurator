#include "wtsn_cfg.h"

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

static bool nvs_get_str(const char *key, char *out, size_t sz, const char *dflt) {
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

static int nvs_get_int(const char *key, int dflt) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return dflt;
    int32_t v = dflt;
    esp_err_t err = nvs_get_i32(h, key, &v);
    nvs_close(h);
    return err == ESP_OK ? (int)v : dflt;
}

static void nvs_set_str(const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_set_int(const char *key, int val) {
    nvs_handle_t h;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

bool wtsn_cfg_load(char *device_id, size_t device_id_sz,
                   char *wifi_ssid, size_t ssid_sz,
                   char *wifi_pass, size_t pass_sz,
                   char *mqtt_host, size_t host_sz,
                   int *mqtt_port) {
    (void)device_id; (void)device_id_sz; /* device id defaults are handled in main */
    nvs_handle_t h = 0;
    if (nvs_open(WTSN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_close(h);
    wtsn_strlcpy(wifi_ssid, "", ssid_sz);
    nvs_get_str(KEY_WIFI_SSID, wifi_ssid, ssid_sz, "");
    nvs_get_str(KEY_WIFI_PASS, wifi_pass, pass_sz, "");
    wtsn_strlcpy(mqtt_host, "", host_sz);
    if (!nvs_get_str(KEY_MQTT_HOST, mqtt_host, host_sz, "")
        || mqtt_host[0] == '\0') wtsn_strlcpy(mqtt_host, "192.168.1.100", host_sz);
    *mqtt_port = nvs_get_int(KEY_MQTT_PORT, 1883);
    return true;
}

bool wtsn_cfg_save(const char *device_id, const char *wifi_ssid,
                   const char *wifi_pass, const char *mqtt_host, int mqtt_port) {
    if (device_id) nvs_set_str(KEY_DEVICE_ID, device_id);
    if (wifi_ssid) nvs_set_str(KEY_WIFI_SSID, wifi_ssid);
    if (wifi_pass) nvs_set_str(KEY_WIFI_PASS, wifi_pass);
    if (mqtt_host) nvs_set_str(KEY_MQTT_HOST, mqtt_host);
    nvs_set_int(KEY_MQTT_PORT, mqtt_port);
    return true;
}
