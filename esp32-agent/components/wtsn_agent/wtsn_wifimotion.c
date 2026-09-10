/* WiFiVision - device-free coarse motion detection from the node's ordinary
 * WiFi link (RSSI jitter + optional CSI channel variance). No camera, no PIR.
 * See wtsn_wifimotion.h for the design.
 */

#include "wtsn_wifimotion.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- tunables ---- */
#ifndef WTSN_WFM_SAMPLE_MS
#define WTSN_WFM_SAMPLE_MS 3500     /* short sampling window (motion reviewed) */
#endif
#ifndef WTSN_WFM_CAL_MS
#define WTSN_WFM_CAL_MS     12000   /* initial "empty room" calibration window */
#endif
#ifndef WTSN_WFM_IDLE_MS
#define WTSN_WFM_IDLE_MS    4500    /* debounce: motion stays asserted this long */
#endif
#ifndef WTSN_WFM_RSSI_N
#define WTSN_WFM_RSSI_N     48      /* ring of per-frame RSSI samples per window */
#endif
#define WTSN_WFM_RSSI_MOS  (WTSN_WFM_RSSI_N / 2)

#ifndef WTSN_WFM_RSSI_THRESHOLD
#define WTSN_WFM_RSSI_THRESHOLD 5   /* trigger when RSSI std-dev crosses this dB */
#endif
/* CSI: absolute trigger on sub-carrier variance (scaled by a factor so the
 * typical ESP32-CSI magnitude is ~1..200). Disabled if CSI is not compiled in. */
#define WTSN_WFM_CSI_THRESHOLD 120.0f

static const char *TAG = "wifimotion";

static char g_dev_id[32] = "esp32-01";
static wtsn_mqtt *g_mq = NULL;

/* ---- RSSI spread accumulator (fed from the WiFi task) ----
 * A person moving between the node and its AP makes the RX RSSI of different
 * frames jitter. We keep a ring of the last N per-frame RSSI values plus
 * running sum/sum-of-squares so the population std-dev is O(1) per sample. */
typedef struct {
    int16_t ring[WTSN_WFM_RSSI_N];
    int     idx;
    int     filled;
    int64_t sum;
    int64_t sq;                 /* sum of squares */
    float   last_stddev;
} rssi_stat_t;

static rssi_stat_t g_rssi;

static void rssi_push(int16_t v) {
    if (g_rssi.filled < WTSN_WFM_RSSI_N) {
        g_rssi.ring[g_rssi.filled++] = v;
        g_rssi.sum += v;
        g_rssi.sq += (int64_t)v * v;
        return;
    }
    int16_t old = g_rssi.ring[g_rssi.idx];
    g_rssi.ring[g_rssi.idx] = v;
    g_rssi.idx = (g_rssi.idx + 1) % WTSN_WFM_RSSI_N;
    g_rssi.sum += v - old;
    g_rssi.sq += (int64_t)v * v - (int64_t)old * old;
}

static float rssi_stddev(void) {
    int n = g_rssi.filled;
    if (n < 2) return 0.0f;
    float mean = (float)g_rssi.sum / n;
    float var = (float)g_rssi.sq / n - mean * mean;
    if (var < 0) var = 0;
    return sqrtf(var);
}

/* ---- CSI variance (only compiled when the IDF built CSI support) ----
 * Each frame's CSI is a list of sub-carrier frequency responses (imag, real).
 * We fold every received frame into a small per-carrier accumulator so the
 * results are the mean |h| and the frame-to-frame variance, then compute a
 * single scalar and compare it to the calibrated floor. */
#if CONFIG_ESP_WIFI_CSI_ENABLED
#define WTSN_WFM_CSI_MAXCAR 64
static int      g_csi_car = 0;
static float    g_csi_mean[WTSN_WFM_CSI_MAXCAR];
static float    g_csi_mean2[WTSN_WFM_CSI_MAXCAR];
static uint32_t g_csi_seen = 0;

static void csi_fold(const wifi_csi_info_t *info) {
    int n = info->len / 2;                      /* pairs of [imag, real] */
    if (n <= 0 || n > WTSN_WFM_CSI_MAXCAR) return;
    if (g_csi_car == 0) g_csi_car = n;
    int ncar = (n < g_csi_car) ? n : g_csi_car;
    const int8_t *b = info->buf;
    int off = info->first_word_invalid ? 4 : 0;
    /* Accumulate magnitude and magnitude^2 per usable sub-carrier. Skip a few
     * edge carriers where the hardware historically reports bogus values. */
    for (int i = 2; i < ncar - 2; i++) {
        int p = i * 2 + off;
        if (p + 1 >= info->len) break;
        float re = b[p + 1];
        float im = b[p];
        float mag = sqrtf(re * re + im * im);
        g_csi_mean[i] += mag;
        g_csi_mean2[i] += mag * mag;
    }
    g_csi_seen++;
}

static void csi_reset_window(void) {
    memset(g_csi_mean, 0, sizeof(g_csi_mean));
    memset(g_csi_mean2, 0, sizeof(g_csi_mean2));
    g_csi_seen = 0;
}

/* Scalar for the current window: average across carriers of the frame variance
 * (E[m^2] - E[m]^2). Motion -> variance rises well above the static floor. */
static float csi_scalar(void) {
    if (g_csi_seen < 2) return 0.0f;
    float tot = 0.0f;
    int count = 0;
    int n = g_csi_car ? g_csi_car : WTSN_WFM_CSI_MAXCAR;
    for (int i = 2; i < n - 2; i++) {
        float mean = g_csi_mean[i] / g_csi_seen;
        float mean2 = g_csi_mean2[i] / g_csi_seen;
        float var;
        if (g_csi_seen > 1) {
            float s = g_csi_seen;
            var = (g_csi_mean2[i] - (g_csi_mean[i] * g_csi_mean[i]) / s) / s;
            if (var < 0) var = 0;
        } else {
            var = mean2 - mean * mean;
            if (var < 0) var = 0;
        }
        tot += var;
        count++;
    }
    return count ? tot / count : 0.0f;
}
#endif /* CONFIG_ESP_WIFI_CSI_ENABLED */

/* ---- windowed decision logic ---- */
typedef struct {
    int64_t window_start;      /* monotonic us */
    int64_t cal_start;
    float   cal_rssi_std;      /* calibrated "empty room" RSSI std-dev */
    float   cal_csi_mag;       /* calibrated CSI scalar floor */
    bool    cal_done;
    bool    active;            /* current motion state */
    int     latched;           /* while asserting, keep reporting motion */
    int64_t latch_until;
    int     last_report;       /* last 0/1 published */
} wm_state_t;

static wm_state_t g_st;

static void wm_publish_event(int motion) {
    if (!g_mq || motion == g_st.last_report) return;
    g_st.last_report = motion;
    char ev[96];
    snprintf(ev, sizeof(ev),
             "{\"id\":\"%s\",\"wifi_motion\":%d,\"type\":\"wifi\",\"rssi_std\":%.1f}",
             g_dev_id, motion, g_rssi.last_stddev);
    wtsn_mqtt_publish(g_mq, "tsn/sensors/event", ev);
    wtsn_mqtt_publish(g_mq, "tsn/fx/data", ev);
    ESP_LOGI(TAG, "wifi motion -> %s (rssi_std=%.1f)", motion ? "DETECTED" : "rest",
             g_rssi.last_stddev);
}

/* CSI callback runs in the WiFi task (per docs). Only fold scalars; the heavy
 * decision work is deferred to wm_tick() in normal task context. */
static void wm_csi_cb(void *ctx, wifi_csi_info_t *info) {
    (void)ctx;
#if CONFIG_ESP_WIFI_CSI_ENABLED
    if (info && info->buf && info->len) csi_fold(info);
#else
    (void)info;
#endif
}

/* Called from the periodic tick — this is where detection decisions happen. */
static void wm_tick(void) {
    int64_t now = esp_timer_get_time();
    if (g_st.window_start == 0) return;          /* not started yet */
    if (now - g_st.window_start < WTSN_WFM_SAMPLE_MS * 1000LL) return;

    float rssi_std = rssi_stddev();
    g_rssi.last_stddev = rssi_std;
    float csi = 0.0f;
#if CONFIG_ESP_WIFI_CSI_ENABLED
    csi = csi_scalar();
    csi_reset_window();
#endif

    /* Calibration: learn the "empty room" floor over the first window(s). */
    if (!g_st.cal_done) {
        if (g_st.cal_start == 0) g_st.cal_start = now;
        if (now - g_st.cal_start >= WTSN_WFM_CAL_MS * 1000LL) {
            g_st.cal_rssi_std = rssi_std * 0.9f;
#if CONFIG_ESP_WIFI_CSI_ENABLED
            g_st.cal_csi_mag = csi * 1.3f + 2.0f;
#else
            g_st.cal_csi_mag = 0.0f;
#endif
            g_st.cal_done = true;
            ESP_LOGI(TAG, "calibrated: rssi_std floor=%.1f dB (csi=%.1f)", rssi_std, csi);
        }
        g_st.window_start = now;                 /* keep sampling during cal */
        return;
    }

    /* Detection: RSSI std-dev relative to the calibrated floor, and/or the
     * absolute CSI variance crossing its (calibrated) threshold. */
    int move = 0;
    if (!(g_rssi.filled >= WTSN_WFM_RSSI_MOS)) {
        move = 0;                                /* not enough data yet */
    } else if (rssi_std > g_st.cal_rssi_std + WTSN_WFM_RSSI_THRESHOLD) {
        move = 1;
    }
#if CONFIG_ESP_WIFI_CSI_ENABLED
    if (!move && csi > g_st.cal_csi_mag + WTSN_WFM_CSI_THRESHOLD)
        move = 1;
#endif

    if (move) {
        g_st.active = true;
        g_st.latched = 1;
        g_st.latch_until = now + WTSN_WFM_IDLE_MS * 1000LL;
    } else if (g_st.latched && now < g_st.latch_until) {
        move = 1;
    } else {
        g_st.active = false;
        g_st.latched = 0;
    }

    if (move != g_st.last_report) {
        wm_publish_event(move);
    }
    g_st.window_start = now;
}

/* Promiscuous RX sink: extract the per-frame RSSI of received data frames and
 * fold it into the ring. Runs in the WiFi task so it must stay cheap — no
 * logging, no allocation. (Control/MGMT frames add channel-noise that would
 * mask the human-body signature, so only data frames are considered.) */
static void wifi_rssi_sink(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA) return;
    /* The same rx_ctrl header is at the front of the promiscuous frame buffer. */
    const wifi_pkt_rx_ctrl_t *rx = (const wifi_pkt_rx_ctrl_t *)buf;
    if (rx->rx_state != 0) return;             /* keep only clean frames */
    rssi_push((int16_t)rx->rssi);
}

void wtsn_wifimotion_init(const char *device_id, wtsn_mqtt *mq) {
    if (device_id) snprintf(g_dev_id, sizeof(g_dev_id), "%s", device_id);
    g_mq = mq;

    /* Subscribe to the RX path so every data frame the radio decodes lands in
     * the RSSI ring. The callback is lightweight by design (C-callable). */
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(wifi_rssi_sink);
    esp_wifi_set_promiscuous(true);

#if CONFIG_ESP_WIFI_CSI_ENABLED
    wifi_csi_config_t cfg = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = 0,
        .dump_ack_en = false,     /* folded ACK frames are tiny but noisy; skip */
    };
    esp_err_t er = esp_wifi_set_csi_rx_cb(&wm_csi_cb, NULL);
    if (er == ESP_OK) {
        esp_wifi_set_csi_config(&cfg);
        esp_wifi_set_csi(true);
        ESP_LOGI(TAG, "wifi sensor: CSI + RSSI-jitter active (CSI enabled)");
    } else {
        ESP_LOGW(TAG, "wifi sensor: CSI unavailable (err=%d), RSSI-jitter only", er);
    }
#else
    ESP_LOGI(TAG, "wifi sensor: RSSI-jitter active (CSI disabled in menuconfig)");
#endif

    memset(&g_st, 0, sizeof(g_st));
    g_st.window_start = esp_timer_get_time();
    g_st.last_report = -1;
    g_st.cal_start = 0;
    g_st.cal_done = false;
}

void wtsn_wifimotion_tick(void) {
    wm_tick();
}

int wtsn_wifimotion_motion(void) {
    return g_st.active ? 1 : 0;
}

float wtsn_wifimotion_rssi_std(void) {
    return g_rssi.last_stddev;
}

void wtsn_wifimotion_recalibrate(void) {
    g_st.cal_done = false;
    g_st.cal_start = esp_timer_get_time();
    g_st.active = false;
    g_st.latched = 0;
#if CONFIG_ESP_WIFI_CSI_ENABLED
    csi_reset_window();
#endif
    ESP_LOGI(TAG, "recalibrating wifi motion sensor");
}
