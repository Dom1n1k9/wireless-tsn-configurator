#include "wtsn_uart.h"
#include "wtsn_mqtt.h"

#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "uart_link";

#define LINK_UART     UART_NUM_1
#define LINK_RXD     GPIO_NUM_14   /* micro:bit pin0 -> ESP RXD1 (3.3V logic) */
#define LINK_TXD     GPIO_NUM_15   /* future use */
#define LINK_BAUD    115200

static wtsn_mqtt *g_mqtt = NULL;
static char g_dev_id[32] = {0};

static void publish_line(const char *line) {
    if (!g_mqtt) return;
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"mb-%s\",\"sensors\":[{\"sensor_id\":\"microbit_serial\","
             "\"type\":4,\"value\":\"%s\",\"unit\":\"\",\"healthy\":1}]}",
             g_dev_id, line);
    wtsn_mqtt_publish(g_mqtt, "tsn/sensors", payload);
}

static void uart_task(void *arg) {
    (void)arg;
    char linebuf[128]; size_t pos = 0;
    uint8_t b;
    for (;;) {
        int n = uart_read_bytes(LINK_UART, &b, 1, pdMS_TO_TICKS(50));
        if (n <= 0) continue;
        if (b == '\n') { if (pos) { linebuf[pos] = 0; publish_line(linebuf); } pos = 0; }
        else if (pos < sizeof(linebuf) - 1) linebuf[pos++] = (char)b;
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
}
