#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"

#include "wtsn_agent.h"
#include "wtsn_cfg.h"
#include "wtsn_mqtt.h"
#include "wtsn_tsn.h"

static const char *TAG = "wtsn_main";

static char g_device_id[32] = "esp32-01";
static wtsn_mqtt *g_mqtt = NULL;

static void on_command(const char *topic, const char *payload, void *ud) {
    (void)ud;
    ESP_LOGI(TAG, "cmd %s <- %s", topic, payload);
    char *slash = strrchr(topic, '/');
    const char *cmd = slash ? slash + 1 : topic;

    if (strcmp(cmd, "qos") == 0) {
        int prio = atoi(payload);
        wtsn_tsn_apply_qos(prio, prio, 0, 0, 0);
    } else if (strcmp(cmd, "vlan") == 0) {
        wtsn_tsn_apply_vlan(atoi(payload), "");
    } else if (strcmp(cmd, "timesync") == 0) {
        wtsn_tsn_apply_timesync(atoi(payload), "");
    } else if (strcmp(cmd, "tas") == 0) {
        wtsn_tsn_apply_tas((int64_t)atoi(payload), "", NULL, NULL, 0);
    } else if (strcmp(cmd, "preemption") == 0) {
        char mode[8] = {0}, emac[32] = {0}, pmac[32] = {0};
        const char *p1 = payload;
        const char *c1 = strchr(p1, ',');
        if (c1) {
            strncpy(mode, p1, (size_t)(c1 - p1) < sizeof(mode) ? (size_t)(c1 - p1) : sizeof(mode) - 1);
            const char *p2 = c1 + 1;
            const char *c2 = strchr(p2, ',');
            if (c2) {
                strncpy(emac, p2, (size_t)(c2 - p2) < sizeof(emac) ? (size_t)(c2 - p2) : sizeof(emac) - 1);
                wtsn_strlcpy(pmac, c2 + 1, sizeof(pmac));
            } else {
                wtsn_strlcpy(emac, p2, sizeof(emac));
            }
        } else {
            wtsn_strlcpy(mode, p1, sizeof(mode));
        }
        wtsn_tsn_apply_preemption(atoi(mode), emac, pmac);
    } else if (strcmp(cmd, "status") == 0) {
        wtsn_tsn_state *st = wtsn_tsn_get_state();
        (void)st;
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"status\":\"online\"}", g_device_id);
        wtsn_mqtt_publish(g_mqtt, "tsn/status", buf);
    } else if (strcmp(cmd, "fx") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "tsn/fx/%s", g_device_id);
        wtsn_mqtt_publish(g_mqtt, buf, payload);
    }
}

static void wifi_init(const char *ssid, const char *pass) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wc = {
        .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK },
    };
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi connecting to %s", ssid);
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    char wifi_ssid[64] = {0};
    char wifi_pass[64] = {0};
    char mqtt_host[64] = {0};
    int mqtt_port = 1883;
    wtsn_cfg_load(g_device_id, sizeof(g_device_id),
                  wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass),
                  mqtt_host, sizeof(mqtt_host), &mqtt_port);

    wifi_init(wifi_ssid[0] ? wifi_ssid : "WTSN", wifi_pass);

    g_mqtt = wtsn_mqtt_create(mqtt_host, mqtt_port, g_device_id, on_command, NULL);
    if (g_mqtt) wtsn_mqtt_start(g_mqtt);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
