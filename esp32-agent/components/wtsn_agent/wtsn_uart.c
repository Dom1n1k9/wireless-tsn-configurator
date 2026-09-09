#include "wtsn_uart.h"
#include "wtsn_mqtt.h"
#include "wtsn_sensor.h"
#include "wtsn_ptp.h"

#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

static const char *TAG = "uart_link";

#define LINK_UART     UART_NUM_1
#define LINK_RXD     GPIO_NUM_14   /* micro:bit pin0 -> ESP RXD1 (3.3V logic) */
#define LINK_TXD     GPIO_NUM_15   /* ESP -> micro:bit pin1 RX: sensor values */
#define LINK_BAUD    115200
/* how often the sensor values are pushed to the micro:bit display */
#define MB_PUSH_MS   1000

/* Every UART frame is terminated by "*<CRC16-CCITT hex>" so a corrupted byte
 * on the wire is detected instead of showing wrong values on the micro:bit
 * panel / mb_* MQTT sensors. Frames without a trailing CRC (legacy peer) are
 * accepted for compatibility but never sent. */
#define CRC16_POLY 0x1021u

static uint16_t crc16_ccitt(const char *data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(uint8_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ CRC16_POLY)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Returns 1 if the frame passes its trailing "*XXXX" CRC check, 0 otherwise.
 * Frames without a "*" are treated as legacy (accepted). */
static int frame_ok(const char *line) {
    const char *star = strrchr(line, '*');
    if (!star) return 1;
    unsigned expect = 0;
    if (sscanf(star + 1, "%4x", &expect) != 1) return 0;
    return crc16_ccitt(line, (size_t)(star - line)) == (uint16_t)expect;
}

static wtsn_mqtt *g_mqtt = NULL;
static char g_dev_id[32] = {0};
static bool g_sync_mode = false;   /* true = this node talks gPTP sync to the panel */
static int g_actor_mode = 0;

static void uart_handle_line(const char *line);

void wtsn_uart_send_line(const char *line) {
    if (!line || !line[0]) return;
    /* Append the CRC trailer: "<line>*<XXXX>\n". The line already includes the
     * trailing '\n' but the CRC must cover only the payload. */
    size_t base = strlen(line);
    if (base && line[base - 1] == '\n') base -= 1;
    char framed[160];
    uint16_t crc = crc16_ccitt(line, base);
    snprintf(framed, sizeof(framed), "%.*s*%04X\n", (int)base, line, crc);
    uart_write_bytes(LINK_UART, framed, (unsigned)strlen(framed));
}

/* Push data to the micro:bit panel:
 *  - sensor node (esp32-01): sensor values  T/P/H/L/M/A
 *  - relay/sync node (esp32-02): gPTP sync   O:<offset> J:<jitter> S:<state> A:<actor>
 */
static void mb_push_task(void *arg) {
    (void)arg;
    char line[80];
    for (;;) {
        if (g_sync_mode) {
            wtsn_ptp_report *r = wtsn_ptp_get_report();
            int64_t off = r ? r->offset_ns : 0;
            int64_t jit = r ? r->jitter_ns : 0;
            int st = r ? r->state : 2;
            snprintf(line, sizeof(line), "O:%lld J:%lld S:%d A:%d\n",
                     (long long)off, (long long)jit, st, g_actor_mode);
        } else {
            float temp = 0, press = 0, hum = 0;
            int light = 0, pir = 0, actor = 0;
            wtsn_sensor_last(&temp, &press, &hum, &light, &pir, &actor);
            snprintf(line, sizeof(line), "T:%.1f P:%.1f H:%.0f L:%d M:%d A:%d\n",
                     temp, press, hum, light, pir, actor);
        }
        wtsn_uart_send_line(line);
        vTaskDelay(pdMS_TO_TICKS(MB_PUSH_MS));
    }
}

static void uart_task(void *arg) {
    (void)arg;
    char linebuf[128]; size_t pos = 0;
    uint8_t b;
    for (;;) {
        int n = uart_read_bytes(LINK_UART, &b, 1, pdMS_TO_TICKS(50));
        if (n <= 0) continue;
        if (b == '\n') {
            if (pos) { linebuf[pos] = 0; uart_handle_line(linebuf); }
            pos = 0;
        } else if (pos < sizeof(linebuf) - 1) linebuf[pos++] = (char)b;
        /* diagnostic: dump raw RX bytes so we can verify the micro:bit line */
        if (pos == 1) {
            ESP_LOGW(TAG, "UART-RX first=%02x", (unsigned)b);
        }
    }
}

/* Handle one complete line from the micro:bit:
 *  - sensor readback lines  "T:.. L:.. P:.."  -> republished as mb_* sensors
 *  - button/command lines   "C:<cmd>"          -> executed on this node
 */
static void uart_handle_line(const char *line) {
    if (!line || !line[0]) return;
    if (!frame_ok(line)) return;

    /* Command from the micro:bit buttons: "C:identify", "C:actor", "C:reboot" */
    if (strncmp(line, "C:", 2) == 0) {
        const char *cmd = line + 2;
        if (strcmp(cmd, "identify") == 0) {
            wtsn_sensor_buzzer_beep(880, 200);
        } else if (strcmp(cmd, "actor") == 0) {
            int cur = wtsn_sensor_actor_get();
            wtsn_sensor_actor_set(cur ? 0 : 1);
        } else if (strcmp(cmd, "reboot") == 0) {
            esp_restart();
        }
        ESP_LOGI(TAG, "micro:bit command: %s", cmd);
        return;
    }

    /* Sensor readback (mb_* telemetry):
     *  - sensor mode: "T:<temp> L:<light> P:<motion> N:<sound>"
     *  - sync mode (esp32-02): "O:<off> J:<jit> S:<state> N:<sound>"
     */
    float t = 0, l = 0, p = 0, s = 0, o = 0, j = 0;
    int st = -1;
    const char *np = strstr(line, "N:");
    if (np) s = (float)atof(np + 2);

    char payload[400];
    if (strncmp(line, "O:", 2) == 0) {
        /* sync-mode readback: publish sound + sync state */
        sscanf(line, "O:%f J:%f S:%d", &o, &j, &st);
        if (st < 0) st = 2;
        snprintf(payload, sizeof(payload),
                 "{\"id\":\"%s\",\"sensors\":["
                 "{\"sensor_id\":\"mb_sound\",\"type\":4,\"value\":%.0f,\"unit\":\"\",\"healthy\":1},"
                 "{\"sensor_id\":\"mb_sync_offset\",\"type\":4,\"value\":%.0f,\"unit\":\"ns\",\"healthy\":1},"
                 "{\"sensor_id\":\"mb_sync_jitter\",\"type\":4,\"value\":%.0f,\"unit\":\"ns\",\"healthy\":1},"
                 "{\"sensor_id\":\"mb_sync_state\",\"type\":4,\"value\":%d,\"unit\":\"\",\"healthy\":1}]}",
                 g_dev_id, s, o, j, st);
        wtsn_mqtt_publish(g_mqtt, "tsn/sensors", payload);
        return;
    }

    if (sscanf(line, "T:%f L:%f P:%f", &t, &l, &p) < 2) return;
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"sensors\":["
             "{\"sensor_id\":\"mb_temp\",\"type\":0,\"value\":%.1f,\"unit\":\"C\",\"healthy\":1},"
             "{\"sensor_id\":\"mb_light\",\"type\":4,\"value\":%.0f,\"unit\":\"\",\"healthy\":1},"
             "{\"sensor_id\":\"mb_pir\",\"type\":4,\"value\":%.0f,\"unit\":\"\",\"healthy\":1},"
             "{\"sensor_id\":\"mb_sound\",\"type\":4,\"value\":%.0f,\"unit\":\"\",\"healthy\":1}]}",
             g_dev_id, t, l, p, s);
    wtsn_mqtt_publish(g_mqtt, "tsn/sensors", payload);
}

void wtsn_uart_init(wtsn_mqtt *mqtt, const char *device_id) {
    g_mqtt = mqtt;
    snprintf(g_dev_id, sizeof(g_dev_id), "%s", device_id ? device_id : "esp32");
    /* esp32-02 has no sensor board - it talks gPTP sync to the panel instead
     * of sensor values, so the micro:bit shows how the two ESPs are synced. */
    g_sync_mode = (strstr(g_dev_id, "esp32-02") != NULL);
    uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(LINK_UART, 1024, 0, 0, NULL, 0);
    uart_param_config(LINK_UART, &cfg);
    uart_set_pin(LINK_UART, LINK_TXD, LINK_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "wired micro:bit UART link ready on GPIO%d (mode=%s)",
             LINK_RXD, g_sync_mode ? "sync" : "sensor");
}

void wtsn_uart_start(void) {
    xTaskCreatePinnedToCore(&uart_task, "wtsn_uart", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(&mb_push_task, "wtsn_mb_tx", 4096, NULL, 5, NULL, 1);
}
