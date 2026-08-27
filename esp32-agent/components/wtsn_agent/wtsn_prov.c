#include "wtsn_prov.h"
#include "wtsn_cfg.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "prov";

#define PROV_AP_SSID "WTSN-Setup"
#define PROV_AP_PASS ""

static bool have_wifi(void) {
    nvs_handle_t h;
    char v[64] = {0};
    if (nvs_open("wtsn", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(v);
    esp_err_t err = nvs_get_str(h, "wifi_ssid", v, &len);
    nvs_close(h);
    return err == ESP_OK && v[0] != '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode application/x-www-form-urlencoded value (in-place). */
static void url_decode(char *s) {
    char *r = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            int hi = hex_val(s[1]);
            int lo = hex_val(s[2]);
            if (hi >= 0 && lo >= 0) {
                *r++ = (char)((hi << 4) | lo);
                s += 3;
                continue;
            }
        }
        if (*s == '+') { *r++ = ' '; s++; continue; }
        *r++ = *s++;
    }
    *r = '\0';
}

static esp_err_t handle_config(httpd_req_t *req) {
    char body[512] = {0};
    int len = req->content_len;
    if (len > (int)sizeof(body) - 1) len = (int)sizeof(body) - 1;
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    body[got] = '\0';

    char ssid[64] = {0}, mqtt[64] = {0}, pass[64] = {0}, devid[32] = {0};
    char *save2 = NULL;
    char *k = strtok_r(body, "&", &save2);
    while (k) {
        char copy[128];
        snprintf(copy, sizeof(copy), "%s", k);
        char *eq = strchr(copy, '=');
        if (!eq) { k = strtok_r(NULL, "&", &save2); continue; }
        *eq = '\0';
        char *key = copy;
        char *val = eq + 1;
        url_decode(val);
        if (strcmp(key, "ssid") == 0) snprintf(ssid, sizeof(ssid), "%s", val);
        else if (strcmp(key, "pass") == 0) snprintf(pass, sizeof(pass), "%s", val);
        else if (strcmp(key, "mqtt") == 0) snprintf(mqtt, sizeof(mqtt), "%s", val);
        else if (strcmp(key, "devid") == 0) snprintf(devid, sizeof(devid), "%s", val);
        k = strtok_r(NULL, "&", &save2);
    }
    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    if (mqtt[0] == '\0') snprintf(mqtt, sizeof(mqtt), "192.168.1.100");
    wtsn_cfg_save(devid[0] ? devid : NULL, ssid, pass, mqtt, 1883);

    char ok[160];
    snprintf(ok, sizeof(ok),
             "<html><body><h1>OK</h1><p>wifi='%s' -> connecting, restart.</p></body></html>",
             ssid);
    httpd_resp_sendstr(req, ok);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req) {
    char cur_dev[32] = {0};
    size_t dev_sz = sizeof(cur_dev);
    if (wtsn_cfg_load_device_id(cur_dev, &dev_sz) && cur_dev[0]) {
        /* current id present */
    }
    char html[1024];
    int n = snprintf(html, sizeof(html),
        "<!doctype html><html><head><meta charset=utf-8><title>WTSN Setup</title></head>"
        "<body style='font-family:sans-serif;max-width:420px;margin:40px auto'>"
        "<h1>WTSN Node Setup</h1>"
        "<form method='POST' action='/config'>"
        "<p><label>Device ID <input name='devid' placeholder='leave empty to keep' value='%s'></label></p>"
        "<p><label>WiFi SSID <input name='ssid' required></label></p>"
        "<p><label>WiFi password <input name='pass' type='password'></label></p>"
        "<p><label>MQTT broker host <input name='mqtt' value='192.168.1.100'></label></p>"
        "<button>Save &amp; connect</button>"
        "</form></body></html>", cur_dev);
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}

static void start_httpd(void) {
    httpd_handle_t server = NULL;
    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.lru_purge_enable = true;
    if (httpd_start(&server, &conf) == ESP_OK) {
        httpd_uri_t r1 = { .uri = "/", .method = HTTP_GET,
                           .handler = handle_root, .user_ctx = NULL };
        httpd_uri_t r2 = { .uri = "/config", .method = HTTP_POST,
                           .handler = handle_config, .user_ctx = NULL };
        httpd_register_uri_handler(server, &r1);
        httpd_register_uri_handler(server, &r2);
        ESP_LOGI(TAG, "provisioning portal ready on http://192.168.4.1/");
    }
}

static bool g_netif_ready = false;
static void ensure_netif(void) {
    if (g_netif_ready) return;
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    g_netif_ready = true;
}

/* Bring up the provisioning SoftAP on an ALREADY-initialised wifi stack (STA from
   the normal boot path already exists). We only add the AP netif and switch to APSTA
   so we do not double-create the STA netif. */
static void ap_on_existing(void) {
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {
        .ap = {
            .ssid = PROV_AP_SSID,
            .ssid_len = strlen(PROV_AP_SSID),
            .password = PROV_AP_PASS,
            .max_connection = 4,
            .authmode = PROV_AP_PASS[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    ESP_LOGI(TAG, "SoftAP '%s' started; connect and open http://192.168.4.1/",
             PROV_AP_SSID);
}

static void configure_ap(void) {
    wifi_config_t ap = {
        .ap = {
            .ssid = PROV_AP_SSID,
            .ssid_len = strlen(PROV_AP_SSID),
            .password = PROV_AP_PASS,
            .max_connection = 4,
            .authmode = PROV_AP_PASS[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    ESP_LOGI(TAG, "SoftAP '%s' started; connect and open http://192.168.4.1/",
             PROV_AP_SSID);
}

void wtsn_prov_start(void) {
    if (have_wifi()) {
        ESP_LOGI(TAG, "WiFi already configured in NVS -> skip provisioning");
        return;
    }
    ensure_netif();
    configure_ap();
    start_httpd();
}

/* Start the provisioning SoftAP + portal even when WiFi is already stored in NVS.
   Used as a fallback when the device cannot reach the saved network, so the user can
   re-provision to a new network without flashing. The STA from the normal boot path is
   left intact (APSTA), and the AP netif is created only once. */
void wtsn_prov_start_ap(void) {
    if (have_wifi())
        ESP_LOGW(TAG, "fallback provisioning AP starting (wifi already set in NVS)");
    ap_on_existing();
    start_httpd();
}
