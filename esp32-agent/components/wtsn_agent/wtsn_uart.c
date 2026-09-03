#include "wtsn_uart.h"
#include "wtsn_mqtt.h"
#include "wtsn_sensor.h"

#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
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

static void publish_line(const char *line) {
    if (!g_mqtt) return;
    /* Drop frames that failed their CRC check (micro:bit's own readback). */
    if (!frame_ok(line)) return;
    /* The micro:bit is a display add-on of THIS node, so publish its onboard
     * sensors under the node's own id (mb_* sensor ids) - a separate
     * "mb-<id>" device would show up as a phantom node in the webgui. */
    float t = 0, l = 0, p = 0;
    if (sscanf(line, "T:%f L:%f P:%f", &t, &l, &p) < 2) return;
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"sensors\":["
             "{\"sensor_id\":\"mb_temp\",\"type\":0,\"value\":%.1f,\"unit\":\"C\",\"healthy\":1},"
             "{\"sensor_id\":\"mb_light\",\"type\":4,\"value\":%.0f,\"unit\":\"\",\"healthy\":1},"
             "{\"sensor_id\":\"mb_pir\",\"type\":4,\"value\":%.0f,\"unit\":\"\",\"healthy\":1}]}",
             g_dev_id, t, l, p);
    wtsn_mqtt_publish(g_mqtt, "tsn/sensors", payload);
}

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

/* Push the cached sensor readings to the micro:bit so it can display them and
 * beep on motion. Line format (one per second):
 *   T:<C> P:<hPa> H:<%RH> L:<lx> M:<0|1> A:<actor mode>   e.g. "T:21.5 P:1013.2 H:48 L:1234 M:0 A:0\n"
 */
static void mb_push_task(void *arg) {
    (void)arg;
    char line[80];
    for (;;) {
        float temp = 0, press = 0, hum = 0;
        int light = 0, pir = 0, actor = 0;
        wtsn_sensor_last(&temp, &press, &hum, &light, &pir, &actor);
        snprintf(line, sizeof(line), "T:%.1f P:%.1f H:%.0f L:%d M:%d A:%d\n",
                 temp, press, hum, light, pir, actor);
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
            if (pos) { linebuf[pos] = 0; publish_line(linebuf); }
            pos = 0;
        } else if (pos < sizeof(linebuf) - 1) linebuf[pos++] = (char)b;
    }
}

void wtsn_uart_init(wtsn_mqtt *mqtt, const char *device_id) {
    g_mqtt = mqtt;
    snprintf(g_dev_id, sizeof(g_dev_id), "%s", device_id ? device_id : "esp32");
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
    ESP_LOGI(TAG, "wired micro:bit UART link ready on GPIO%d", LINK_RXD);
}

void wtsn_uart_start(void) {
    xTaskCreatePinnedToCore(&uart_task, "wtsn_uart", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(&mb_push_task, "wtsn_mb_tx", 4096, NULL, 5, NULL, 1);
}
