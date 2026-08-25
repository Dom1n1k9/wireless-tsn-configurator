#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"
#include "stdio.h"

#include "wtsn_cfg.h"
#include "wtsn_mqtt.h"
#include "wtsn_tsn.h"
#include "wtsn_ptp.h"
#include "wtsn_json.h"
#include "wtsn_prov.h"

static const char *TAG = "wtsn_main";
static char g_device_id[32] = "esp32-01";
static wtsn_mqtt *g_mqtt = NULL;

static void send_ack(bool ok, const char *reason) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"ok\":%s", g_device_id,
                    ok ? "true" : "false");
    if (!ok && reason && reason[0]) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, ",\"err\":\"%s\"", reason);
    }
    snprintf(buf + n, sizeof(buf) - (size_t)n, "}");
    char topic[40];
    snprintf(topic, sizeof(topic), "tsn/ack/%s", g_device_id);
    wtsn_mqtt_publish(g_mqtt, topic, buf);
}

static void crate_set_wifi(const char *payload) {
    char ssid[64] = {0};
    char pass[64] = {0};
    wtsn_json_get_str(payload, "ssid", ssid, sizeof(ssid));
    wtsn_json_get_str(payload, "pass", pass, sizeof(pass));
    if (ssid[0]) {
        wtsn_cfg_set_wifi(ssid, pass);
        ESP_LOGI(TAG, "set_wifi: %s (restart to connect)", ssid);
        /* Restart will apply the new credentials from NVS on boot. */
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

static void on_connected(const char *client_id, void *ud) {
    (void)client_id; (void)ud;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"id\":\"%s\"}", g_device_id);
    wtsn_mqtt_publish(g_mqtt, "tsn/discover", buf);
    ESP_LOGI(TAG, "published discover for %s", g_device_id);
}

static void apply_snapshot(const char *payload) {
    if (!payload || !payload[0] || payload[0] != '{') {
        send_ack(false, "bad_json");
        return;
    }
    wtsn_config_snapshot cfg;
    memset(&cfg, 0, sizeof(cfg));
    int v;
    bool have_prio = wtsn_json_get_int(payload, "priority", &v);
    if (have_prio) cfg.priority = v;
    if (wtsn_json_get_int(payload, "traffic_class", &v)) cfg.traffic_class = v;
    if (wtsn_json_get_int(payload, "bandwidth_kbps", &v)) cfg.bandwidth_kbps = v;
    if (wtsn_json_get_int(payload, "latency_ms", &v)) cfg.latency_ms = v;
    if (wtsn_json_get_int(payload, "preemption", &v)) cfg.preemption = v;
    if (wtsn_json_get_int(payload, "vlan_id", &v)) cfg.vlan_id = v;
    wtsn_json_get_str(payload, "group", cfg.group, sizeof(cfg.group));
    if (wtsn_json_get_int(payload, "timesync_mode", &v)) cfg.timesync_mode = v;
    wtsn_json_get_str(payload, "grandmaster", cfg.grandmaster, sizeof(cfg.grandmaster));
    int64_t l;
    if (wtsn_json_get_i64(payload, "tas_cycle_ns", &l)) cfg.tas_cycle_ns = l;

    /* parse optional GCL array (802.1Qbv gate windows), if present */
    const char *gcl_arr = NULL;
    wtsn_json_get_root_array(payload, "gcl", &gcl_arr);
    if (gcl_arr) {
        cfg.gcl_entries = wtsn_json_parse_gcl(gcl_arr, cfg.gates, cfg.durations,
                                               WTSN_GCL_MAX);
    }

    if (have_prio && (cfg.priority < 0 || cfg.priority > 7)) {
        send_ack(false, "bad_priority");
        return;
    }
    if (cfg.preemption < 0 || cfg.preemption > 1) {
        send_ack(false, "bad_preemption");
        return;
    }
    if (cfg.timesync_mode < 0 || cfg.timesync_mode > 3) {
        send_ack(false, "bad_timesync_mode");
        return;
    }

    if (wtsn_tsn_apply_snapshot(&cfg) == 0) send_ack(true, "");
    else send_ack(false, "apply_failed");
}

static void on_command(const char *topic, const char *payload, void *ud) {
    (void)ud;
    ESP_LOGI(TAG, "cmd %s <- %s", topic, payload);
    if (strstr(topic, "/apply")) { apply_snapshot(payload); return; }

    char *slash = strrchr(topic, '/');
    const char *cmd = slash ? slash + 1 : topic;

    if (strcmp(cmd, "wifi") == 0) {
        /* set_wifi: expects JSON {"ssid":"...","pass":"..."} on tsn/cmd/<id>/wifi */
        crate_set_wifi(payload);
        send_ack(true, "");
    } else if (strcmp(cmd, "qos") == 0) {
        int p = atoi(payload);
        wtsn_tsn_apply_qos(p, p, 0, 0, 0);
        send_ack(true, "");
    } else if (strcmp(cmd, "vlan") == 0) {
        wtsn_tsn_apply_vlan(atoi(payload), "");
        send_ack(true, "");
    } else if (strcmp(cmd, "timesync") == 0) {
        wtsn_tsn_apply_timesync(atoi(payload), "");
        send_ack(true, "");
    } else if (strcmp(cmd, "tas") == 0) {
        wtsn_tsn_apply_tas((int64_t)atoi(payload), NULL, NULL, 0);
        send_ack(true, "");
    } else if (strcmp(cmd, "stream") == 0) {
        apply_snapshot(payload);
        send_ack(true, "");
    } else if (strcmp(cmd, "preemption") == 0) {
        char mode[8] = {0}, emac[32] = {0}, pmac[32] = {0};
        const char *p1 = payload;
        const char *c1 = strchr(p1, ',');
        if (c1) {
            size_t ml = (size_t)(c1 - p1);
            strncpy(mode, p1, ml < sizeof(mode) ? ml : sizeof(mode) - 1);
            const char *p2 = c1 + 1;
            const char *c2 = strchr(p2, ',');
            if (c2) {
                size_t el = (size_t)(c2 - p2);
                strncpy(emac, p2, el < sizeof(emac) ? el : sizeof(emac) - 1);
                wtsn_strlcpy(pmac, c2 + 1, sizeof(pmac));
            } else {
                wtsn_strlcpy(emac, p2, sizeof(emac));
            }
        } else {
            wtsn_strlcpy(mode, p1, sizeof(mode));
        }
        wtsn_tsn_apply_preemption(atoi(mode), emac, pmac);
        send_ack(true, "");
    } else if (strcmp(cmd, "status") == 0) {
        wtsn_tsn_state *st = wtsn_tsn_get_state();
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"id\":\"%s\",\"status\":\"online\",\"prio\":%d,\"vlan\":%d,"
                 "\"preempt\":%d,\"tc\":%d}",
                 g_device_id, st->priority, st->vlan_id, st->preemption, st->traffic_class);
        wtsn_mqtt_publish(g_mqtt, "tsn/status", buf);
    } else if (strcmp(cmd, "fx") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "tsn/fx/%s", g_device_id);
        wtsn_mqtt_publish(g_mqtt, buf, payload);
    }
}

typedef struct {
    char ssid[64];
    char password[64];
    char host[64];
    int  port;
} wifi_ctx_t;

static wifi_ctx_t g_ctx;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    wifi_ctx_t *ctx = (wifi_ctx_t *)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
        ESP_LOGI(TAG, "wifi connecting to %s", ctx->ssid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* will retry automatically via reconnect if enabled; reconnect manually */
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        if (!g_mqtt) {
            g_mqtt = wtsn_mqtt_create(ctx->host, ctx->port, g_device_id,
                                       on_command, on_connected, NULL);
            if (g_mqtt) wtsn_mqtt_start(g_mqtt);
            wtsn_ptp_setup(g_device_id, g_mqtt);
            wtsn_ptp_start();
            ESP_LOGI(TAG, "agent %s broker %s:%d", g_device_id, ctx->host, ctx->port);
        }
    }
}

/* Blink onboard LED 3x at startup so a reboot/flash is visibly confirmed. */
static void blink_led(void) {
    gpio_config_t io = {0};
    io.pin_bit_mask = (1ULL << GPIO_NUM_2);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    for (int i = 0; i < 3; i++) {
        gpio_set_level(GPIO_NUM_2, 1);
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_set_level(GPIO_NUM_2, 0);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void wifi_init(const char *ssid, const char *pass) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    snprintf(g_ctx.ssid, sizeof(g_ctx.ssid), "%s", ssid);
    snprintf(g_ctx.password, sizeof(g_ctx.password), "%s", pass);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, &g_ctx, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, &g_ctx, NULL));

    wifi_config_t wc = {
        .sta = {
            .ssid = "",
            .password = "",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    blink_led();

    char wifi_ssid[64] = {0};
    char wifi_pass[64] = {0};
    char mqtt_host[64] = {0};
    int mqtt_port = 1883;
    wtsn_cfg_load(g_device_id, sizeof(g_device_id),
                  wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass),
                  mqtt_host, sizeof(mqtt_host), &mqtt_port);

    if (!wifi_ssid[0]) {
        ESP_LOGW(TAG, "no WiFi in NVS -> entering provisioning mode");
        wtsn_prov_start();
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    snprintf(g_ctx.host, sizeof(g_ctx.host), "%s", mqtt_host);
    g_ctx.port = mqtt_port;

    wifi_init(wifi_ssid, wifi_pass);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
