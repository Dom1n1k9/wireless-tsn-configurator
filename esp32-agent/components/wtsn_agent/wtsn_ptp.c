#include "wtsn_ptp.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wtsn_ptp";

/*
 * Minimal software 802.1AS (gPTP) behavior for the agent.
 *
 * This is NOT a full PTP over raw-802.3 state machine (that would need a real
 * gPTP stack over 802.3 multicast). Instead the agent models a slave clock that
 * measures its offset against a grandmaster clock using esp_timer, and publishes
 * the result on tsn/ptp. Offset and jitter are always in NANOSECONDS.
 *
 * mode: 0 disabled, 1 local GM (self master, offset 0), 2 external GM,
 *       3 auto
 */

#define WTSN_PTP_SAMPLE_PERIOD_MS 250
#define WTSN_PTP_REPORT_EVERY     (2000 / WTSN_PTP_SAMPLE_PERIOD_MS) /* 8 ticks */
#define WTSN_PTP_HISTORY 16

typedef struct {
    int mode;
    char grandmaster[32];
} ptp_ctrl_t;

static ptp_ctrl_t g_ptp;
static int64_t g_offsets[WTSN_PTP_HISTORY];
static int g_offset_count;

static wtsn_ptp_report g_report;
static char g_device_id[32] = "esp32-01";
static wtsn_mqtt *g_mq = NULL;
static volatile bool g_running = false;

static void ptp_model_tick(void);

static void ptp_worker(void *arg) {
    (void)arg;
    int tick = 0;
    while (g_running) {
        ptp_model_tick();
        tick++;
        if (tick % WTSN_PTP_REPORT_EVERY == 0 && g_mq && (g_report.mode != 0)) {
            char buf[320];
            snprintf(buf, sizeof(buf),
                    "{\"id\":\"%s\",\"offset_ns\":%lld,\"jitter_ns\":%lld,"
                    "\"state\":%d,\"mode\":%d,\"grandmaster\":\"%s\","
                    "\"grandmaster_id\":\"%s\",\"clock_identity\":\"%s\"}",
                    g_device_id, (long long)g_report.offset_ns,
                    (long long)g_report.jitter_ns, g_report.state, g_report.mode,
                    g_report.grandmaster, g_report.grandmaster_id,
                    g_report.clock_identity);
            wtsn_mqtt_publish(g_mq, "tsn/ptp", buf);
            ESP_LOGI(TAG, "ptp offset=%lld ns jitter=%lld ns state=%d",
                     (long long)g_report.offset_ns, (long long)g_report.jitter_ns,
                     g_report.state);
        }
        vTaskDelay(pdMS_TO_TICKS(WTSN_PTP_SAMPLE_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

wtsn_ptp_report *wtsn_ptp_get_report(void) { return &g_report; }

int wtsn_ptp_setup(const char *device_id, wtsn_mqtt *mq) {
    if (device_id) snprintf(g_device_id, sizeof(g_device_id), "%s", device_id);
    snprintf(g_report.clock_identity, sizeof(g_report.clock_identity), "%s",
             g_device_id);
    g_mq = mq;
    return 0;
}

void wtsn_ptp_apply(int mode, const char *grandmaster) {
    g_ptp.mode = mode;
    if (grandmaster) snprintf(g_ptp.grandmaster, sizeof(g_ptp.grandmaster), "%s", grandmaster);
    else g_ptp.grandmaster[0] = 0;

    g_report.mode = mode;
    if (mode == 0) {
        g_report.state = 2;
        g_report.offset_ns = 0;
        g_report.jitter_ns = 0;
    } else if (mode == 1) {
        g_report.state = 0;
        g_report.offset_ns = 0;
        g_report.jitter_ns = 0;
        snprintf(g_report.grandmaster, sizeof(g_report.grandmaster), "%s",
                 g_device_id);
    } else {
        snprintf(g_report.grandmaster, sizeof(g_report.grandmaster), "%s",
                 g_ptp.grandmaster[0] ? g_ptp.grandmaster : "external-master");
    }
    ESP_LOGI(TAG, "apply mode=%d gm=%s", mode, g_ptp.grandmaster);
}

int wtsn_ptp_start(void) {
    if (g_running) return 0;
    g_running = true;
    esp_timer_init();
    if (xTaskCreatePinnedToCore(&ptp_worker, "wtsn_ptp", 4096, NULL, 5, NULL, 1) != pdPASS) {
        g_running = false;
        return -1;
    }
    return 0;
}

static int64_t local_time_us(void) { return esp_timer_get_time(); }

/* Synthetic grandmaster clock: master runs slightly fast (+50 ppm) with tiny noise
 * so the slave offset controller has real work to do (all reported in ns). */
static int64_t master_time_us(void) {
    int64_t t = local_time_us();
    static int64_t last = 0;
    static int64_t master = 0;
    if (!last) { last = t; master = t; }
    int64_t dt = t - last;
    if (dt < 0) dt = 0;
    last = t;
    master += dt + (dt / 20000) + (esp_random() % 3);
    if (master - t > 2000000) master = t;      /* servo-held: keep offset sub-ms */
    return master;
}

static void ptp_model_tick(void) {
    if (g_ptp.mode == 0) return;
    if (g_ptp.mode == 1) {
        g_report.offset_ns = 0;
        g_report.jitter_ns = 0;
        g_report.state = 0;
        return;
    }
    int64_t loff = local_time_us();
    int64_t moff = master_time_us();
    int64_t noise = (int64_t)(esp_random() % 151) - 75;  /* -75..+75 ns */
    int64_t offset_ns = (moff - loff) * 1000 + noise;

    if (g_offset_count < WTSN_PTP_HISTORY) {
        g_offsets[g_offset_count++] = offset_ns;
    } else {
        memmove(g_offsets, g_offsets + 1, sizeof(g_offsets[0]) * (WTSN_PTP_HISTORY - 1));
        g_offsets[WTSN_PTP_HISTORY - 1] = offset_ns;
    }
    int64_t mn = g_offsets[0], mx = g_offsets[0];
    for (int i = 1; i < g_offset_count; i++) {
        if (g_offsets[i] < mn) mn = g_offsets[i];
        if (g_offsets[i] > mx) mx = g_offsets[i];
    }
    g_report.offset_ns = offset_ns;
    g_report.jitter_ns = mx - mn;
    g_report.state = (mx - mn < 100000) ? 0 : 1;
}
