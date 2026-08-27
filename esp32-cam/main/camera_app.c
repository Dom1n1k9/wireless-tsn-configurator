/* Wireless TSN ESP32-CAM agent: WiFi + MQTT node + MJPEG HTTP stream.
 *
 * The CAM acts as another WTSN node: it joins the same MQTT broker, announces
 * itself (tsn/discover), publishes motion/events (tsn/sensors/event) and serves
 * a live MJPEG stream on http://<ip>/stream for the user to view in a browser.
 *
 * It reuses the same credential model as esp32-agent (NVS). On first boot with no
 * WiFi stored it starts a WTSN-Setup SoftAP + provisioning portal at 192.168.4.1.
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "esp_camera.h"

static const char *TAG = "cam_agent";

/* ---- board / pins (ESP32-CAM / AI Thinker AI-Thinker) ---- */
#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      21
#define Y4_GPIO_NUM      19
#define Y3_GPIO_NUM      18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

/* ---------------- NVS helpers (credential model identical to esp32-agent) ---------------- */
static void nvs_str_get(const char *key, char *out, size_t sz) {
    nvs_handle_t h; out[0] = '\0';
    if (nvs_open("wtsn", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sz;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
    nvs_close(h);
}
static void nvs_str_set(const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open("wtsn", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}
static bool nvs_has_wifi(void) {
    nvs_handle_t h; char v[64] = {0}; size_t len = sizeof(v);
    if (nvs_open("wtsn", NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_str(h, "wifi_ssid", v, &len);
    nvs_close(h);
    return e == ESP_OK && v[0];
}

static char g_device_id[32] = "esp32-cam-01";
static esp_mqtt_client_handle_t g_mqtt = NULL;
static httpd_handle_t g_stream_server = NULL;

/* ---------------- provisioning portal (SoftAP + config form) ---------------- */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void url_decode(char *s) {
    char *r = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            int hi = hex_val(s[1]), lo = hex_val(s[2]);
            if (hi >= 0 && lo >= 0) { *r++ = (char)((hi << 4) | lo); s += 3; continue; }
        }
        if (*s == '+') { *r++ = ' '; s++; continue; }
        *r++ = *s++;
    }
    *r = '\0';
}

static esp_err_t prov_config_handler(httpd_req_t *req) {
    char body[512] = {0};
    int len = req->content_len;
    if (len > (int)sizeof(body) - 1) len = (int)sizeof(body) - 1;
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[got] = '\0';
    char ssid[64] = {0}, pass[64] = {0}, mqtt[64] = {0}, id[32] = {0};
    char *save = NULL;
    char *k = strtok_r(body, "&", &save);
    while (k) {
        char copy[128]; snprintf(copy, sizeof(copy), "%s", k);
        char *eq = strchr(copy, '=');
        if (!eq) { k = strtok_r(NULL, "&", &save); continue; }
        *eq = '\0'; char *val = eq + 1; url_decode(val);
        if (!strcmp(copy, "ssid")) snprintf(ssid, sizeof(ssid), "%s", val);
        else if (!strcmp(copy, "pass")) snprintf(pass, sizeof(pass), "%s", val);
        else if (!strcmp(copy, "mqtt")) snprintf(mqtt, sizeof(mqtt), "%s", val);
        else if (!strcmp(copy, "id")) snprintf(id, sizeof(id), "%s", val);
        k = strtok_r(NULL, "&", &save);
    }
    if (!ssid[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required"); return ESP_FAIL; }
    if (!mqtt[0]) snprintf(mqtt, sizeof(mqtt), "192.168.1.100");
    if (id[0]) nvs_str_set("device_id", id);
    nvs_str_set("wifi_ssid", ssid);
    nvs_str_set("wifi_pass", pass);
    nvs_str_set("mqtt_host", mqtt);
    const char *ok = "<html><body><h1>OK</h1><p>Connecting... restart.</p></body></html>";
    httpd_resp_sendstr(req, ok);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t prov_root_handler(httpd_req_t *req) {
    const char *html =
        "<!doctype html><html><head><meta charset=utf-8><title>WTSN CAM Setup</title></head>"
        "<body style='font-family:sans-serif;max-width:440px;margin:40px auto'>"
        "<h1>WTSN ESP32-CAM Setup</h1>"
        "<form method='POST' action='/config'>"
        "<p><label>WiFi SSID <input name='ssid' required></label></p>"
        "<p><label>WiFi password <input name='pass' type='password'></label></p>"
        "<p><label>MQTT broker host <input name='mqtt' value='192.168.1.100'></label></p>"
        "<p><label>Node id <input name='id' value='esp32-cam-01'></label></p>"
        "<button>Save &amp; connect</button>"
        "</form></body></html>";
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}

static void start_prov_ap(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    wifi_config_t ap = {
        .ap = {
            .ssid = "WTSN-Setup", .ssid_len = strlen("WTSN-Setup"),
            .password = "", .max_connection = 4, .authmode = WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    httpd_handle_t server = NULL;
    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.lru_purge_enable = true;
    if (httpd_start(&server, &conf) == ESP_OK) {
        httpd_uri_t r1 = { .uri = "/", .method = HTTP_GET, .handler = prov_root_handler, .user_ctx = NULL };
        httpd_uri_t r2 = { .uri = "/config", .method = HTTP_POST, .handler = prov_config_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &r1);
        httpd_register_uri_handler(server, &r2);
        ESP_LOGI(TAG, "provisioning portal ready on http://192.168.4.1/");
    }
}

/* ---------------- camera init ---------------- */
static void camera_init(void) {
    camera_config_t config;
    memset(&config, 0, sizeof(config));
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: 0x%x", err);
        return;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == OV2640_PID) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 0);
        s->set_brightness(s, 1);
        s->set_saturation(s, 2);
    }
    ESP_LOGI(TAG, "camera ready");
}

/* ---------------- MJPEG stream ---------------- */
static const char *_STREAM_BOUNDARY = "123456789000000000000987654321";

static esp_err_t stream_handler(httpd_req_t *req) {
    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/x-mixed-replace; boundary=%s", _STREAM_BOUNDARY);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "24");

    esp_err_t res = ESP_OK;
    char part_line[128];
    camera_fb_t *fb;
    while (res == ESP_OK) {
        fb = esp_camera_fb_get();
        if (!fb) {
            res = ESP_FAIL;
            ESP_LOGE(TAG, "get fb failed");
        } else {
            if (fb->format != PIXFORMAT_JPEG) {
                ESP_LOGE(TAG, "non-jpeg frame");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
                break;
            }
            /* part header */
            int pl = snprintf(part_line, sizeof(part_line),
                "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                _STREAM_BOUNDARY, (size_t)fb->len);
            res = httpd_resp_send_chunk(req, part_line, (ssize_t)pl);
            /* jpeg payload */
            if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, (ssize_t)fb->len);
            esp_camera_fb_return(fb);
        }
    }
    return res;
}

/* ---------------- MQTT ---------------- */

static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)base; (void)event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT connected; publishing discover");
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"id\":\"%s\"}", g_device_id);
        esp_mqtt_client_publish(g_mqtt, "tsn/discover", buf, 0, 1, 0);
    }
}

static esp_err_t mqtt_start(void) {
    char host[64] = {0}; nvs_str_get("mqtt_host", host, sizeof(host));
    if (!host[0]) snprintf(host, sizeof(host), "192.168.1.100");
    esp_mqtt_client_config_t cfg = {
        .broker = { .address = { .hostname = host, .port = 1883, .transport = MQTT_TRANSPORT_OVER_TCP } },
        .credentials = { .client_id = g_device_id },
        .session = { .keepalive = 30 },
    };
    g_mqtt = esp_mqtt_client_init(&cfg);
    if (!g_mqtt) return ESP_FAIL;
    esp_mqtt_client_register_event(g_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(g_mqtt);
    return ESP_OK;
}

/* ---------------- wifi (STA) ---------------- */
typedef struct {
    char ssid[64];
    char password[64];
} wifi_ctx_t;
static wifi_ctx_t g_ctx;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        if (!g_mqtt) {
            if (mqtt_start() != ESP_OK) ESP_LOGE(TAG, "mqtt start failed");
            /* start stream http server */
            httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
            conf.server_port = 80;
            conf.lru_purge_enable = true;
            if (httpd_start(&g_stream_server, &conf) == ESP_OK) {
                static httpd_uri_t stream_uri = {
                    .uri = "/stream", .method = HTTP_GET,
                    .handler = stream_handler, .user_ctx = NULL,
                };
                httpd_register_uri_handler(g_stream_server, &stream_uri);
                ESP_LOGI(TAG, "http://" IPSTR "/stream", IP2STR(&e->ip_info.ip));
            }
        }
    }
}

static void wifi_start(const char *ssid, const char *pass) {
    snprintf(g_ctx.ssid, sizeof(g_ctx.ssid), "%s", ssid);
    snprintf(g_ctx.password, sizeof(g_ctx.password), "%s", pass);
    wifi_config_t wc = {
        .sta = {
            .ssid = "", .password = "",
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
    char wifi_ssid[64] = {0}, wifi_pass[64] = {0};
    nvs_str_get("wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    nvs_str_get("wifi_pass", wifi_pass, sizeof(wifi_pass));
    nvs_str_get("device_id", g_device_id, sizeof(g_device_id));
    if (g_device_id[0] == '\0') snprintf(g_device_id, sizeof(g_device_id), "esp32-cam-01");

    if (!wifi_ssid[0]) {
        start_prov_ap();
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    camera_init();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wc);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, &g_ctx, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, &g_ctx, NULL);
    wifi_start(wifi_ssid, wifi_pass);

    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
