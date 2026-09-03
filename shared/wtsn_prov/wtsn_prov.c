#include "wtsn_prov.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "prov";

/* SoftAP password when no NVS override is set. Leave empty for an open AP
 * (the historical default). To lock the provisioning AP down, set a
 * non-empty password here OR store one under the NVS key "ap_pass" (namespace
 * "wtsn") -- the NVS value wins. */
#define PROV_AP_PASS_DEFAULT ""

#define PROV_AP_PASS_MAX 64

static struct {
    const char *title;
    const char *default_mqtt;
    wtsn_prov_save_fn save_cb;
    wtsn_prov_load_id_fn load_id_cb;
} g_cfg = {
    .title = "WTSN Setup",
    .default_mqtt = "wtsn-broker.local",
};

void wtsn_prov_init(const char *portal_title, const char *default_mqtt,
                    wtsn_prov_save_fn save_cb, wtsn_prov_load_id_fn load_id_cb) {
    if (portal_title) g_cfg.title = portal_title;
    if (default_mqtt) g_cfg.default_mqtt = default_mqtt;
    g_cfg.save_cb = save_cb;
    g_cfg.load_id_cb = load_id_cb;
}

bool wtsn_prov_have_wifi(void) {
    nvs_handle_t h;
    char v[64] = {0};
    if (nvs_open("wtsn", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(v);
    esp_err_t err = nvs_get_str(h, "wifi_ssid", v, &len);
    nvs_close(h);
    return err == ESP_OK && v[0] != '\0';
}

static void load_current_id(char *out, size_t sz) {
    out[0] = '\0';
    if (g_cfg.load_id_cb) g_cfg.load_id_cb(out, sz);
}

/* Build a unique SSID so multiple boards do not all broadcast identical "WTSN-Setup"
 * APs (which makes them indistinguishable). SSID = "WTSN-Setup-<shortid>". */
static char g_ap_ssid[32] = "WTSN-Setup";
static const char *ap_ssid(void) {
    if (g_ap_ssid[0] && strcmp(g_ap_ssid, "WTSN-Setup") != 0) return g_ap_ssid;
    char dev[32] = {0};
    load_current_id(dev, sizeof(dev));
    if (dev[0]) {
        /* strip leading non-alnum chars and cap length so SSID stays <=32 bytes */
        char tail[16] = {0};
        const char *p = dev;
        size_t t = 0;
        for (; *p && t < sizeof(tail) - 1; p++) {
            char c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_') tail[t++] = c;
        }
        tail[t] = '\0';
        if (tail[0]) snprintf(g_ap_ssid, sizeof(g_ap_ssid), "WTSN-Setup-%s", tail);
    }
    return g_ap_ssid;
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
        /* both "devid" (agent) and "id" (cam) are accepted */
        if (strcmp(key, "ssid") == 0) snprintf(ssid, sizeof(ssid), "%s", val);
        else if (strcmp(key, "pass") == 0) snprintf(pass, sizeof(pass), "%s", val);
        else if (strcmp(key, "mqtt") == 0) snprintf(mqtt, sizeof(mqtt), "%s", val);
        else if (strcmp(key, "devid") == 0 || strcmp(key, "id") == 0)
            snprintf(devid, sizeof(devid), "%s", val);
        k = strtok_r(NULL, "&", &save2);
    }
    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    if (mqtt[0] == '\0') snprintf(mqtt, sizeof(mqtt), "wtsn-broker.local");
    if (!g_cfg.save_cb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no save handler");
        return ESP_FAIL;
    }
    g_cfg.save_cb(ssid, pass, devid, mqtt);

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
    load_current_id(cur_dev, sizeof(cur_dev));
    char html[1024];
    snprintf(html, sizeof(html),
        "<!doctype html><html><head><meta charset=utf-8><title>%s</title></head>"
        "<body style='font-family:sans-serif;max-width:420px;margin:40px auto'>"
        "<h1>%s</h1>"
        "<form method='POST' action='/config'>"
        "<p><label>Device ID <input name='devid' placeholder='leave empty to keep' value='%s'></label></p>"
        "<p><label>WiFi SSID <input name='ssid' required></label></p>"
        "<p><label>WiFi password <input name='pass' type='password'></label></p>"
        "<p><label>MQTT broker host <input name='mqtt' value='%s'></label></p>"
        "<button>Save &amp; connect</button>"
        "</form></body></html>",
        g_cfg.title, g_cfg.title, cur_dev, g_cfg.default_mqtt);
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

static void configure_ap(void) {
    /* Resolve the SoftAP password: an NVS "ap_pass" override wins over the
     * compile-time default. A non-empty password puts the AP behind WPA2-PSK
     * so credentials entered into the portal are not exposed to the open air. */
    char ap_pass[PROV_AP_PASS_MAX] = {0};
    nvs_handle_t ah = 0;
    if (nvs_open("wtsn", NVS_READONLY, &ah) == ESP_OK) {
        size_t plen = sizeof(ap_pass) - 1;
        if (nvs_get_str(ah, "ap_pass", ap_pass, &plen) != ESP_OK) {
            snprintf(ap_pass, sizeof(ap_pass), "%s", PROV_AP_PASS_DEFAULT);
        }
        nvs_close(ah);
    } else {
        snprintf(ap_pass, sizeof(ap_pass), "%s", PROV_AP_PASS_DEFAULT);
    }

    wifi_config_t ap = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    const char *ssid = ap_ssid();
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", ssid);
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    if (ap_pass[0]) {
        if (strlen(ap_pass) < 8) {
            ESP_LOGW(TAG, "ap_pass too short (<8 chars) -> keeping AP open");
        } else {
            snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", ap_pass);
            ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        }
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    ESP_LOGI(TAG, "SoftAP '%s' started (auth=%s); connect and open http://192.168.4.1/",
             ssid, ap.ap.authmode == WIFI_AUTH_WPA2_PSK ? "WPA2" : "open");
}

/* Bring up the provisioning SoftAP on an ALREADY-initialised wifi stack (STA from
 * the normal boot path already exists). Only add the AP netif and switch to APSTA
 * so we do not double-create the STA netif. */
static void ap_on_existing(void) {
    esp_netif_create_default_wifi_ap();
    configure_ap();
}

void wtsn_prov_start(void) {
    if (wtsn_prov_have_wifi()) {
        ESP_LOGI(TAG, "WiFi already configured in NVS -> skip provisioning");
        return;
    }
    ensure_netif();
    configure_ap();
    start_httpd();
}

/* Start the provisioning SoftAP + portal even when WiFi is already stored in NVS.
 * Used as a fallback when the device cannot reach the saved network, so the user can
 * re-provision to a new network without flashing. The STA from the normal boot path is
 * left intact (APSTA), and the AP netif is created only once. */
void wtsn_prov_start_ap(void) {
    if (wtsn_prov_have_wifi())
        ESP_LOGW(TAG, "fallback provisioning AP starting (wifi already set in NVS)");
    ap_on_existing();
    start_httpd();
}
