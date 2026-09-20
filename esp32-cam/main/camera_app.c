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
#include "wtsn_prov.h"
#include "wtsn_version.h"
#include "wtsn_ota.h"
#include "sntp.h"

#include "driver/sdmmc_host.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "freertos/event_groups.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "mdns.h"

#include <dirent.h>

#include <time.h>

static const char *TAG = "cam_agent";

/* ---- microSD recording (motion-triggered clips) ----
 * The AI-Thinker ESP32-CAM has an onboard microSD slot wired to the VSPI bus:
 *   CS=GPIO13, SCK=GPIO14, MOSI=GPIO15, MISO=GPIO2
 * Recording uses only this bus, so it does not collide with the camera pins.
 * While a clip is being shot we keep the live /stream working too (the camera
 * driver serves both consumers from its PSRAM frame buffers). */
#define SD_HOST_ID        SPI2_HOST
#define SD_PIN_CS         13
#define SD_PIN_SCK        14
#define SD_PIN_MOSI       15
#define SD_PIN_MISO       2
#define SD_MOUNT_POINT    "/sdcard"
#define RECORD_MS         30000            /* clip length: 30 s */
#define REC_LED_GPIO      GPIO_NUM_33      /* on-board flash/LED for "recording" */

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
static char g_device_id[32] = "esp32-cam-01";
static char g_ip[16] = "0.0.0.0";
static bool g_sntp_started = false;
static esp_mqtt_client_handle_t g_mqtt = NULL;
static httpd_handle_t g_stream_server = NULL;

/* ---- motion-triggered recording state ----
 * g_rec_active  : 1 while a 30 s clip is being captured.
 * g_motion_seen : debounced latch set by an FX/motion event; recording is started
 *                 on the next camera-frame tick so we do not grab in an interrupt. */
static volatile int  g_motion_seen = 0;
static volatile int  g_rec_active = 0;
static bool          g_sd_mounted = false;
static int           g_rec_index = 0;      /* monotonic clip counter for filenames */

/* Last recorded clip metadata so the GUI can replay it ("Play" button):
 * offsets/lengths of every JPEG frame inside g_last_clip_path. Kept in RAM for
 * the session; on reboot just point the GUI at the SD card name again. */
#define LAST_CLIP_MAX_FRAMES 2400
static char    g_last_clip_path[96];
static int     g_last_clip_frames = 0;
static uint32_t g_last_offs[LAST_CLIP_MAX_FRAMES];
static uint32_t g_last_lens[LAST_CLIP_MAX_FRAMES];

/* ---------------- provisioning (shared component: shared/wtsn_prov) ---------------- */
static void cam_prov_save(const char *ssid, const char *pass,
                          const char *devid, const char *mqtt) {
    if (devid && devid[0]) nvs_str_set("device_id", devid);
    nvs_str_set("wifi_ssid", ssid);
    nvs_str_set("wifi_pass", pass);
    nvs_str_set("mqtt_host", mqtt);
}

static bool cam_prov_load_id(char *out, size_t sz) {
    nvs_str_get("device_id", out, sz);
    if (!out[0]) snprintf(out, sz, "esp32-cam-01");
    return out[0] != '\0';
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
    config.fb_count = 3;
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

/* ---------------- recording LED ---------------- */
static void rec_led_set(bool on) {
    static bool cfg = false;
    if (!cfg) {
        gpio_config_t io = {0};
        io.pin_bit_mask = (1ULL << REC_LED_GPIO);
        io.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io);
        cfg = true;
    }
    gpio_set_level(REC_LED_GPIO, on ? 1 : 0);
}

static void rec_led_task(void *arg) {
    (void)arg;
    bool on = false;
    for (;;) {
        /* blink while recording, off after */
        if (g_rec_active) {
            on = !on;
            rec_led_set(on);
        } else {
            if (on) { rec_led_set(false); on = false; }
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

/* ---------------- microSD mount ---------------- */
static void sd_init(void) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST_ID;
    host.max_freq_khz = 20000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32768,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi_bus_init failed: 0x%x", err);
        return;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_PIN_CS;
    slot_cfg.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mcfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: 0x%x (no recording)", err);
        return;
    }
    g_sd_mounted = true;
    ESP_LOGI(TAG, "SD mounted: %llu MB", (unsigned long long)card->csd.capacity / 1024ULL);
}

/* ---------------- motion-triggered recording ---------------- */

/* Event payload from esp32-01 sensor node arrives on tsn/fx/data and
 * tsn/sensors/event as  {"id":"esp32-01","motion":1,...}  or as a short JSON
 * {"motion":1,"raw":1}. Any frame whose motion/wifi_motion field is non-zero
 * starts a clip. */
static bool json_motion_true(const char *payload) {
    if (!payload) return false;
    const char *m = strstr(payload, "\"motion\"");
    if (!m) m = strstr(payload, "\"wifi_motion\"");
    if (m) {
        const char *col = strchr(m, ':');
        if (col) {
            const char *v = col + 1;
            while (*v == ' ') v++;
            if (*v == '1') return true;
        }
    }
    return false;
}

static void mqtt_motion_event(const char *topic, const char *payload) {
    (void)topic;
    if (!payload) return;
    if (json_motion_true(payload)) {
        ESP_LOGI(TAG, "motion event via FXMQTT -> recording trigger");
        g_motion_seen = 1;
    }
}

/* Capture one clip: grab camera frames and write each JPEG to a numbered file.
 * Runs while RECORD_MS elapses; stops early if the SD card disappears. */
static void record_task(void *arg) {
    (void)arg;
    if (g_rec_active) { vTaskDelete(NULL); return; }
    g_rec_active = 1;

    char path[96];
    int clip = ++g_rec_index;
    snprintf(path, sizeof(path), "%s/clip_%04d_%lld.jpg", SD_MOUNT_POINT, clip,
             (long long)time(NULL));

    FILE *fout = fopen(path, "wb");
    if (!fout) {
        ESP_LOGE(TAG, "cannot open %s for writing (SD full/unmounted?)", path);
        g_rec_active = 0;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "recording %s for %d ms", path, RECORD_MS);

    /* Keep a small "last recording" preview: the first frame of the clip is
     * mirrored to /sdcard/last.jpg so the web GUI's Devices page can show what
     * was recorded with a cheap HTTP GET (<img src=http://<cam>/last.jpg>). */
    {
        camera_fb_t *fb0 = esp_camera_fb_get();
        if (fb0) {
            if (fb0->format == PIXFORMAT_JPEG && fb0->len) {
                FILE *lp = fopen(SD_MOUNT_POINT "/last.jpg", "wb");
                if (lp) {
                    fwrite(fb0->buf, 1, fb0->len, lp);
                    fclose(lp);
                }
            }
            esp_camera_fb_return(fb0);
        }
    }

    int64_t start = esp_timer_get_time();
    uint32_t running = 0;
    int nframes = 0;
    while (esp_timer_get_time() - start < (int64_t)RECORD_MS * 1000) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (fb->format == PIXFORMAT_JPEG && fb->len) {
            fwrite(fb->buf, 1, fb->len, fout);
            if (nframes < LAST_CLIP_MAX_FRAMES) {
                g_last_offs[nframes] = running;
                g_last_lens[nframes] = fb->len;
                nframes++;
            }
            running += fb->len;
        }
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(30));   /* ~33 fps, throttle writes */
    }
    fclose(fout);
    g_last_clip_frames = nframes;
    snprintf(g_last_clip_path, sizeof(g_last_clip_path), "%s", path);
    ESP_LOGI(TAG, "recording finished: %s (%d frames/%lu B)", path, nframes, (unsigned long)running);
    g_rec_active = 0;

    /* after the clip publish a status so the GUI knows recording ended */
    char t[64], msg[192];
    snprintf(t, sizeof(t), "tsn/status/%s", g_device_id);
    snprintf(msg, sizeof(msg), "{\"id\":\"%s\",\"rec\":\"%s\",\"saved\":1}", g_device_id, path);
    if (g_mqtt) esp_mqtt_client_publish(g_mqtt, t, msg, 0, 0, 0);

    /* Send the current list of saved clips so the web GUI can show "what it
     * recorded" without polling. Payload: {"id":...,"recordings":["/sdcard/a.jpg",...]}
     * (names only; fetching the bytes off the CAM SD is out of scope). */
    {
        char lib[2048];
        int n = snprintf(lib, sizeof(lib), "{\"id\":\"%s\",\"recordings\":[", g_device_id);
        bool first = true;
        DIR *d = opendir(SD_MOUNT_POINT);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                size_t l = strlen(e->d_name);
                if (l < 4 || strcmp(e->d_name + l - 4, ".jpg") != 0) continue;
                n += snprintf(lib + n, sizeof(lib) - (size_t)n, "%s\"/sdcard/%s\"",
                              first ? "" : ",", e->d_name);
                first = false;
            }
            closedir(d);
        }
        snprintf(lib + n, sizeof(lib) - (size_t)n, "]}");
        if (g_mqtt) esp_mqtt_client_publish(g_mqtt, "tsn/cam/recordings", lib, 0, 0, 0);
    }

    vTaskDelete(NULL);
}

void cam_motion_trigger(void) {
    if (!g_sd_mounted) { ESP_LOGW(TAG, "motion but SD not mounted -> skipped"); return; }
    if (g_rec_active)  return;   /* already recording: debounce */
    g_motion_seen = 0;
    xTaskCreatePinnedToCore(record_task, "cam_rec", 8192, NULL, 6, NULL, 1);
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

/* Serve the "last recording" still: /sdcard/last.jpg (updated at the start of
 * every clip). Returns a single JPEG for the Devices page thumbnail. */
static esp_err_t last_handler(httpd_req_t *req) {
    if (!g_sd_mounted) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "no sd", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    FILE *f = fopen(SD_MOUNT_POINT "/last.jpg", "rb");
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "no last frame yet", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char buf[1024];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)rd) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Replay the last recorded clip as an MJPEG stream. Uses the in-RAM offset/length
 * table built while the clip was captured so we never need to parse the JPEG
 * stream - just seek and stream each frame back, then end the multipart stream.
 * If no clip has been recorded yet this session, fall back to the live camera. */
static esp_err_t replay_handler(httpd_req_t *req) {
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/x-mixed-replace; boundary=%s", _STREAM_BOUNDARY);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    FILE *f = NULL;
    if (g_sd_mounted && g_last_clip_frames > 0 && g_last_clip_path[0]) {
        f = fopen(g_last_clip_path, "rb");
        if (!f) f = NULL;
    }
    if (!f) {
        ESP_LOGW(TAG, "replay: no saved clip; falling back to live camera");
        httpd_resp_set_status(req, "200 OK");
        return stream_handler(req);   /* no clip this session -> just live */
    }

    ESP_LOGI(TAG, "replay: %s (%d frames)", g_last_clip_path, g_last_clip_frames);
    char part_line[160];
    char buf[4096];
    for (int i = 0; i < g_last_clip_frames; i++) {
        /* leave a gap roughly matching real-time (~33 fps) */
        vTaskDelay(pdMS_TO_TICKS(28));
        if (fseek(f, (long)g_last_offs[i], SEEK_SET) != 0) break;
        int pl = snprintf(part_line, sizeof(part_line),
            "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n",
            _STREAM_BOUNDARY, (unsigned long)g_last_lens[i]);
        if (httpd_resp_send_chunk(req, part_line, (ssize_t)pl) != ESP_OK) break;
        uint32_t left = g_last_lens[i];
        while (left > 0) {
            size_t want = left < sizeof(buf) ? left : sizeof(buf);
            size_t rd = fread(buf, 1, want, f);
            if (rd == 0) break;
            left -= (uint32_t)rd;
            if (httpd_resp_send_chunk(req, buf, (ssize_t)rd) != ESP_OK) break;
        }
    }
    fclose(f);
    /* terminating chunk ends the multipart stream */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void ota_go(const char *url, const char *crc32_hex);

static void mqtt_data(void *arg, esp_mqtt_event_handle_t e) {
    (void)arg;
    /* The CAM acts on:
     *  - its own OTA command: {"url":"http://<host>/fw/x.bin"}  on tsn/cmd/<id>/ota
     *  - shared FX motion feed  tsn/fx/data  +  tsn/sensors/event
     *    (esp32-01 publishes motion from PIR and WiFiVision) -> record 30 s. */
    char topic[64] = {0};
    int tl = e->topic_len < (int)sizeof(topic) - 1 ? e->topic_len : (int)sizeof(topic) - 1;
    memcpy(topic, e->topic, (size_t)tl);
    if (tl > 3 && strcmp(topic + tl - 4, "/ota") == 0) {
        char *p = (char *)e->data;
        const char *u = strstr(p, "\"url\"");
        if (u) {
            u = strchr(u + 5, ':');
            if (u) {
                u = strchr(u + 1, '"');
                if (u) {
                    const char *v = u + 1;
                    char url[256] = {0};
                    size_t i = 0;
                    for (; v[i] && v[i] != '"' && i < sizeof(url) - 1; i++) {
                        url[i] = v[i];
                    }
                    url[i] = '\0';
                    /* optional "crc32":"<hex>" -> verify image after download */
                    char crc[16] = {0};
                    const char *c = strstr(p, "\"crc32\"");
                    if (c) {
                        c = strchr(c + 7, ':');
                        if (c) {
                            c = strchr(c + 1, '"');
                            if (c) {
                                const char *w = c + 1;
                                size_t j = 0;
                                for (; w[j] && w[j] != '"' && j < sizeof(crc) - 1; j++) {
                                    crc[j] = w[j];
                                }
                                crc[j] = '\0';
                            }
                        }
                    }
                    ota_go(url, crc[0] ? crc : NULL);
                }
            }
        }
        return;
    }
    if (strcmp(topic, "tsn/fx/data") == 0 || strcmp(topic, "tsn/sensors/event") == 0) {
        mqtt_motion_event(topic, (char *)e->data);
    }
}

static void ota_go(const char *url, const char *crc32_hex) {
    if (!url || !url[0]) return;
    char ack_topic[48], ack[64];
    snprintf(ack_topic, sizeof(ack_topic), "tsn/ack/%s", g_device_id);
    snprintf(ack, sizeof(ack), "{\"id\":\"%s\",\"ok\":true}", g_device_id);
    ESP_LOGI(TAG, "OTA command: %s (crc32 %s)", url, crc32_hex ? crc32_hex : "none");
    esp_mqtt_client_publish(g_mqtt, ack_topic, ack, 0, 0, 0);
    wtsn_ota_start_checked(url, crc32_hex);
}

static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)base; (void)handler_args;
    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT connected; publishing discover");
        char t[64];
        snprintf(t, sizeof(t), "tsn/cmd/%s/ota", g_device_id);
        esp_mqtt_client_subscribe(g_mqtt, t, 0);
        /* motion-driven recording: listen to the sensor node's shared feeds */
        esp_mqtt_client_subscribe(g_mqtt, "tsn/fx/data", 0);
        esp_mqtt_client_subscribe(g_mqtt, "tsn/sensors/event", 0);
        esp_mqtt_client_register_event(g_mqtt, MQTT_EVENT_DATA, mqtt_data, NULL);
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"id\":\"%s\",\"fw\":\"%s\",\"ip\":\"%s\",\"kind\":\"cam\"}",
                 g_device_id, WTSN_FW_VERSION, g_ip);
        esp_mqtt_client_publish(g_mqtt, "tsn/discover", buf, 0, 1, 0);
    }
}

/* Resolve a possible ".local" MQTT broker hostname into an IP string (mirrors
 * esp32-agent: lwIP getaddrinfo() cannot answer .local here, so query mDNS
 * explicitly; if that fails fall back to the well-known provisioning PC IP so
 * the CAM still reaches the broker on this LAN). */
static void resolve_mqtt_host(char *host, size_t host_sz) {
    if (!host || !host[0]) return;
    bool is_local = (strstr(host, ".local") != NULL);
    if (!is_local) return;
    char q[64];
    snprintf(q, sizeof(q), "%s", host);
    size_t ql = strlen(q);
    if (ql > 6 && strcmp(q + ql - 6, ".local") == 0) q[ql - 6] = '\0';
    esp_ip4_addr_t addr = {0};
    esp_err_t err = mdns_query_a(q, 2000, &addr);
    if (err == ESP_OK && addr.addr != 0) {
        snprintf(host, host_sz, IPSTR, IP2STR(&addr));
        ESP_LOGI(TAG, "mDNS resolved %s -> %s", q, host);
        return;
    }
    ESP_LOGW(TAG, "mDNS query for %s failed (%d) - keeping '%s'", q, err, host);
    if (strstr(host, ".local") != NULL) {
        snprintf(host, host_sz, "192.168.0.149");
        ESP_LOGW(TAG, "using fallback broker IP %s", host);
    }
}

static esp_err_t mqtt_start(void) {
    char host[64] = {0}; nvs_str_get("mqtt_host", host, sizeof(host));
    if (!host[0]) snprintf(host, sizeof(host), "wtsn-broker.local");
    resolve_mqtt_host(host, sizeof(host));
    /* LWT: broker marks the CAM offline (retained) if it vanishes unexpectedly. */
    char will_topic[48];
    snprintf(will_topic, sizeof(will_topic), "tsn/lwt/%s", g_device_id);
    static const char will_msg[] = "offline";
    /* broker auth: same NVS keys as esp32-agent (portal writes muser/mpass).
     * Static: esp-mqtt keeps the string pointers past this function. */
    static char muser[64], mpass[64];
    nvs_str_get("muser", muser, sizeof(muser));
    nvs_str_get("mpass", mpass, sizeof(mpass));
    esp_mqtt_client_config_t cfg = {
        .broker = { .address = { .hostname = host, .port = 1883, .transport = MQTT_TRANSPORT_OVER_TCP } },
        .credentials = {
            .client_id = g_device_id,
            .username = muser[0] ? muser : NULL,
            .authentication = { .password = mpass[0] ? mpass : NULL },
        },
        .session = { .keepalive = 30,
                     .last_will = {
                         .topic = will_topic,
                         .msg = will_msg,
                         .msg_len = (size_t)strlen(will_msg),
                         .qos = 1,
                         .retain = 1,
                     } },
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

/* Re-provision fallback: after this many consecutive disconnects with no IP, give up
 * retrying the dead network and bring back the WTSN-Setup SoftAP so the CAM can be
 * re-pointed at a new network over the air (no USB flash needed). */
#ifndef PROV_FALLBACK_DISCONNECTS
#define PROV_FALLBACK_DISCONNECTS 6
#endif
static volatile int g_wifi_ready = 0;
static volatile int g_disconnect_streak = 0;
static volatile int g_reprov_started = 0;

static void reprov_task(void *arg) {
    (void)arg;
    if (g_reprov_started) { vTaskDelete(NULL); return; }
    g_reprov_started = 1;
    ESP_LOGW(TAG, "unable to connect on '%s' -> starting provisioning AP", g_ctx.ssid);
    wtsn_prov_start_ap();   /* blocks serving the portal; user config -> restart */
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected, retrying");
        esp_wifi_connect();
        if (++g_disconnect_streak >= PROV_FALLBACK_DISCONNECTS &&
            !g_wifi_ready && !g_reprov_started) {
            xTaskCreatePinnedToCore(&reprov_task, "cam_reprov", 4096, NULL, 5, NULL, 1);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_wifi_ready = 1;
        g_disconnect_streak = 0;
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&e->ip_info.ip));
        if (!g_sntp_started) {
            g_sntp_started = true;
            sntp_setoperatingmode(SNTP_OPMODE_POLL);
            sntp_setservername(0, "pool.ntp.org");
            sntp_init();
        }
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
                static httpd_uri_t last_uri = {
                    .uri = "/last.jpg", .method = HTTP_GET,
                    .handler = last_handler, .user_ctx = NULL,
                };
                httpd_register_uri_handler(g_stream_server, &last_uri);
                static httpd_uri_t replay_uri = {
                    .uri = "/replay.mjpeg", .method = HTTP_GET,
                    .handler = replay_handler, .user_ctx = NULL,
                };
                httpd_register_uri_handler(g_stream_server, &replay_uri);
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

    wtsn_prov_init("WTSN CAM Setup", "wtsn-broker.local", cam_prov_save, cam_prov_load_id);

    if (!wifi_ssid[0]) {
        wtsn_prov_start();
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    camera_init();
    sd_init();

    xTaskCreatePinnedToCore(rec_led_task, "cam_recled", 2048, NULL, 4, NULL, 1);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    /* mDNS resolver so a ".local" broker (wtsn-broker.local) can be resolved;
     * must run after the STA netif exists so multicast goes out over WiFi. */
    mdns_init();
    mdns_hostname_set(g_device_id);
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wc);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, &g_ctx, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, &g_ctx, NULL);
    wifi_start(wifi_ssid, wifi_pass);

    /* main loop: debounce FX motion and fire a 30 s recording when triggered */
    while (1) {
        if (g_motion_seen) {
            /* start a clip if we are not already recording */
            if (!g_rec_active) cam_motion_trigger();
            else g_motion_seen = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
