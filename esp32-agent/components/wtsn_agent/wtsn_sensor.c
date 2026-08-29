#include "wtsn_sensor.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/adc.h"
#include "driver/uart.h"
#include "esp_adc_cal.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "sensor";

/* I2C / GPIO / ADC pin assignments for the WTSN node sensor add-on board. */
#ifndef WTSN_I2C_PORT
#define WTSN_I2C_PORT I2C_NUM_0
#endif
#ifndef WTSN_I2C_SDA
#define WTSN_I2C_SDA GPIO_NUM_21
#endif
#ifndef WTSN_I2C_SCL
#define WTSN_I2C_SCL GPIO_NUM_22
#endif
/* The device that has the sensor add-on board wired publishes telemetry.
 * main.c defaults to "esp32-01". */
#ifndef WTSN_SENSOR_DEV_ID
#define WTSN_SENSOR_DEV_ID "esp32-01"
#endif
#define WTSN_ADC_CH ADC1_CHANNEL_5
/* HC-SR501 PIR motion on GPIO27 */
#ifndef WTSN_PIR_GPIO
#define WTSN_PIR_GPIO GPIO_NUM_27
#endif
/* Actor relay/switch on GPIO26 */
#ifndef WTSN_ACTOR_GPIO
#define WTSN_ACTOR_GPIO GPIO_NUM_26
#endif

/* UART link to a micro:bit display. The ESP sends compact sensor JSON down this TX
 * line (GPIO17 = UART2_TXD); the micro:bit shows it on its LED matrix and sounds
 * its speaker on motion. Wire ESP TX(GPIO17) -> micro:bit P0(RX), share GND. */
#ifndef WTSN_UART_TX
#define WTSN_UART_TX GPIO_NUM_17
#endif
#ifndef WTSN_UART_PORT
#define WTSN_UART_PORT UART_NUM_2
#endif

#define WTSN_I2C_FREQ_HZ 100000
#define BME280_ADDR     0x76
#define BME280_ID_REG   0xD0
#define BME280_CTRL_MEAS 0xF4
#define BME280_CTRL_HUM  0xF2
#define BME280_STATUS    0xF3
#define BME280_CONFIG    0xF5
#define BME280_PRESS_MSB  0xF7
#define BME280_CALIB00_MSB 0x88
#define BME280_CALIB26_MSB 0xE1

static char g_dev_id[32] = "esp32-01";
static wtsn_mqtt *g_mq = NULL;

static bool g_bme_ok = false;
static int  g_prev_pir = -1;
static int  g_actor_mode = 0;
static int64_t g_pir_latch = -1000000000LL;  /* monotonic us of last motion */

static uint16_t calib_temp[3];  /* T1..T3 */
static int32_t  calib_press[9]; /* P1..P9 (P1 is unsigned 16-bit, others signed) */
static uint8_t  calib_hum_H1;
static int16_t  calib_hum_H2;
static int8_t   calib_hum_H3, calib_hum_H6;
static int16_t  calib_hum_H4, calib_hum_H5;

static int64_t g_read_period_ns = 2000000000LL; /* 2s */

/* cached last readings for the BLE panel */
static float g_last_temp = 0, g_last_hum = 0;
static int g_last_light = 0, g_last_pir = 0;

static esp_adc_cal_characteristics_t g_adc_char;
static bool g_adc_cal = false;

/* ---------------- BME280 (I2C) ---------------- */

static esp_err_t bme_reg_read8(uint8_t reg, uint8_t *out) {
    i2c_cmd_handle_t c = i2c_cmd_link_create();
    esp_err_t err;
    i2c_master_start(c);
    i2c_master_write_byte(c, (BME280_ADDR << 1) | I2C_MASTER_WRITE, 1);
    i2c_master_write_byte(c, reg, 1);
    i2c_master_start(c);
    i2c_master_write_byte(c, (BME280_ADDR << 1) | I2C_MASTER_READ, 1);
    i2c_master_read_byte(c, out, I2C_MASTER_NACK);
    i2c_master_stop(c);
    err = i2c_master_cmd_begin(WTSN_I2C_PORT, c, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(c);
    return err;
}

static esp_err_t bme_reg_read(uint8_t reg, uint8_t *buf, size_t n) {
    i2c_cmd_handle_t c = i2c_cmd_link_create();
    esp_err_t err;
    i2c_master_start(c);
    i2c_master_write_byte(c, (BME280_ADDR << 1) | I2C_MASTER_WRITE, 1);
    i2c_master_write_byte(c, reg, 1);
    i2c_master_start(c);
    i2c_master_write_byte(c, (BME280_ADDR << 1) | I2C_MASTER_READ, 1);
    if (n > 1) i2c_master_read(c, buf, n - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(c, buf + n - 1, I2C_MASTER_NACK);
    i2c_master_stop(c);
    err = i2c_master_cmd_begin(WTSN_I2C_PORT, c, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(c);
    return err;
}

static void bme_write8(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t c = i2c_cmd_link_create();
    i2c_master_start(c);
    i2c_master_write_byte(c, (BME280_ADDR << 1) | I2C_MASTER_WRITE, 1);
    i2c_master_write_byte(c, reg, 1);
    i2c_master_write_byte(c, val, 1);
    i2c_master_stop(c);
    i2c_master_cmd_begin(WTSN_I2C_PORT, c, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(c);
}

static int bme280_init(void) {
    uint8_t chip = 0;
    if (bme_reg_read8(BME280_ID_REG, &chip) != ESP_OK || chip != 0x60) {
        ESP_LOGW(TAG, "BME280 not found (chip=0x%02x)", chip);
        return -1;
    }
    uint8_t buf[26];
    if (bme_reg_read(BME280_CALIB00_MSB, buf, 26) != ESP_OK) return -1;
    calib_temp[0] = (uint16_t)((buf[1] << 8) | buf[0]);
    calib_temp[1] = (int16_t)((buf[3] << 8) | buf[2]);
    calib_temp[2] = (int16_t)((buf[5] << 8) | buf[4]);
    calib_press[0] = (uint16_t)((buf[7]  << 8) | buf[6]);
    for (int i = 1; i < 9; i++) {
        /* P-block: after 6 temp bytes, P(i+1) is at msb=6+2i+1, lsb=6+2i */
        int lsb = 6 + 2 * i, msb = lsb + 1;
        calib_press[i] = (int16_t)((buf[msb] << 8) | buf[lsb]);
    }
    ESP_LOGI(TAG, "BME280 calib P1=%ld P2=%ld P3=%ld T1=%u",
             (long)calib_press[0], (long)calib_press[1], (long)calib_press[2],
             (unsigned)calib_temp[0]);
    /* BME280 humidity calibration at 0xE1..0xE8 (8 bytes):
     *  0xE1 = H1 (u8), 0xE2/0xE3 = H2 (s16, lsb/msb),
     *  0xE4 = H3 (u8), 0xE5 = H4 msb (s8)*16, 0xE6 low nibble = H4 lsb,
     *  high nibble = H5 lsb, 0xE7 = H5 msb (s8)*16, 0xE8 = H6 (s8). */
    uint8_t hum_regs[8];
    if (bme_reg_read(0xE1, hum_regs, 8) == ESP_OK) {
        calib_hum_H1 = hum_regs[0];
        calib_hum_H2 = (int16_t)((hum_regs[2] << 8) | hum_regs[1]);
        calib_hum_H3 = (int8_t)hum_regs[3];
        calib_hum_H4 = (int16_t)((hum_regs[4] << 4) | (hum_regs[5] & 0x0F));
        calib_hum_H5 = (int16_t)((hum_regs[5] >> 4) | (hum_regs[6] << 4));
        calib_hum_H6 = (int8_t)hum_regs[7];
    }

    bme_write8(BME280_CTRL_HUM, 0x01);   /* oversampling x1 */
    bme_write8(BME280_CTRL_MEAS, 0x27);  /* t x1, p x1, normal mode */
    bme_write8(BME280_CONFIG, 0xA0);      /* t_sb 1000ms, filter off */
    vTaskDelay(pdMS_TO_TICKS(50));
    g_bme_ok = true;
    ESP_LOGI(TAG, "BME280 init OK");
    return 0;
}

static int bme280_read(float *temp_c, float *press_hpa, float *hum_pct) {
    if (!g_bme_ok) return -1;
    uint8_t buf[8];
    if (bme_reg_read(BME280_PRESS_MSB, buf, 8) != ESP_OK) return -1;
    int32_t adc_p = (int32_t)(((uint32_t)buf[0] << 12) | ((uint32_t)buf[1] << 4) | ((uint32_t)buf[2] >> 4));
    int32_t adc_t = (int32_t)(((uint32_t)buf[3] << 12) | ((uint32_t)buf[4] << 4) | ((uint32_t)buf[5] >> 4));
    int32_t adc_h = (int32_t)(((uint32_t)buf[6] << 8) | (uint32_t)buf[7]);

    int32_t var1 = ((((adc_t >> 3) - ((int32_t)calib_temp[0] << 1)) *
                     ((int32_t)calib_temp[1])) >> 11);
    int32_t var2 = (((((adc_t >> 4) - ((int32_t)calib_temp[0])) *
                      ((adc_t >> 4) - ((int32_t)calib_temp[0]))) >> 12) *
                    ((int32_t)calib_temp[2])) >> 14;
    int32_t t_fine = var1 + var2;
    if (temp_c) *temp_c = (float)((t_fine * 5 + 128) >> 8) / 100.0f;

    if (press_hpa) {
        int64_t v1 = (int64_t)t_fine - 128000;
        int64_t v2 = v1 * v1 * (int64_t)calib_press[5];
        v2 = v2 + ((v1 * (int64_t)calib_press[4]) << 17);
        v2 = v2 + (((int64_t)calib_press[3]) << 35);
        v1 = ((v1 * v1 * (int64_t)calib_press[2]) >> 8) +
             ((v1 * (int64_t)calib_press[1]) << 12);
        v1 = (((((int64_t)1 << 47) + v1)) * (int64_t)calib_press[0]) >> 33;
        if (v1 != 0) {
            int64_t p = ((int64_t)(1048576 - adc_p)) << 31;
            p = ((p - v2) * 3125) / v1;
            int64_t p8 = (((int64_t)calib_press[8]) * ((p >> 13) * (p >> 13))) >> 25;
            int64_t p7 = (((int64_t)calib_press[7]) * p) >> 19;
            int64_t p_fine = ((p + p8 + p7) >> 8) + (((int64_t)calib_press[6]) << 4);
            int64_t press_pa = (p_fine >> 1) * 100 / 128;
            if (press_hpa) *press_hpa = (float)press_pa / 10000.0f;
        }
    }

    if (hum_pct && g_bme_ok) {
        /* Humidity raw + compensation. Use the Bosch double-integer algorithm,
         * but fall back to the direct raw/1024 mapping (=%RH, since the raw
         * value uses a 0..102400 scale for 0..100%RH) in case this module
         * reports atypical calibration (e.g. H2=1) that degenerates the exact
         * compensation to zero. */
        float hum = 0.0f;
        {
            int32_t v1 = t_fine - 76800;
            int32_t v2 = adc_h * 16384;
            int32_t v3 = (int32_t)calib_hum_H4 * 1048576;
            int32_t v4 = (int32_t)calib_hum_H5 * v1;
            int32_t v5 = ((v2 - v3) - v4 + 16384) / 32768;
            v2 = (v1 * (int32_t)calib_hum_H6) / 1024;
            v3 = (v1 * (int32_t)calib_hum_H3) / 2048;
            v4 = ((v2 * (v3 + 32768)) / 1024) + 2097152;
            v2 = ((v4 * (int32_t)calib_hum_H2) + 8192) / 16384;
            v3 = v5 * v2;
            v4 = ((v3 / 32768) * (v3 / 32768)) / 128;
            v5 = v3 - ((v4 * (int32_t)calib_hum_H1) / 16);
            v5 = (v5 < 0) ? 0 : v5;
            if (v5 > 419430400) v5 = 419430400;
            int32_t humidity = v5 / 4096;
            if (humidity > 102400) humidity = 102400;
            hum = (float)humidity / 1024.0f;
        }
        if (hum <= 0.1f) hum = (float)adc_h / 1024.0f;
        if (hum > 100.0f) hum = 100.0f;
        if (hum < 0.0f) hum = 0.0f;
        *hum_pct = hum;
    }
    return 0;
}

/* ---------------- analog light (TEMT6000) ---------------- */

static int light_read_raw(void) {
    if (!g_adc_cal) return 0;
    uint32_t raw = 0;
    adc1_config_channel_atten(WTSN_ADC_CH, ADC_ATTEN_DB_12);
    for (int i = 0; i < 8; i++) raw += adc1_get_raw(WTSN_ADC_CH);
    raw /= 8;
    uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &g_adc_char);
    /* TEMT6000 datasheet: Iphoto ~= 2.8 uA per 100 lx (0.028 uA/lx).
     * With the on-board R1=10k load: V = I * R = 0.28 mV per lx
     *   => lux = mV / 0.28 = mV * 3.6   (Vcc=3.3V, linear region). */
    float lux = (float)mv * 3.6f;
    if (lux < 0) lux = 0;
    return (int)lux;
}

/* ---------------- actor (timer switch, 7 modes) ---------------- */

static void actor_apply(int mode) {
    if (mode < 0) mode = 0;
    if (mode >= TIMER_SWITCH_MODES) mode = TIMER_SWITCH_MODES - 1;
    g_actor_mode = mode;
    int on = (mode == 1 || mode == 2 || mode == 3 || mode == 6) ? 1 : 0;
    gpio_set_level(WTSN_ACTOR_GPIO, on);
    ESP_LOGI(TAG, "actor mode=%d -> out=%d", mode, on);
}

/* ---------------- public API ---------------- */

void wtsn_sensor_actor_set_pin(void) {
    gpio_config_t io = {0};
    io.pin_bit_mask = (1ULL << WTSN_ACTOR_GPIO);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
}

static int already_sent_mb_init = 0;
static void microbit_send(const char *json_line) {
    if (already_sent_mb_init) uart_wait_tx_done(WTSN_UART_PORT, 0);
    uart_write_bytes(WTSN_UART_PORT, json_line, strlen(json_line));
    uart_write_bytes(WTSN_UART_PORT, "\n", 1);
    ESP_LOGI(TAG, "microbit-> %.*s", (int)(strlen(json_line) > 80 ? 80 : strlen(json_line)), json_line);
}

void wtsn_sensor_init(const char *device_id, wtsn_mqtt *mq) {
    if (device_id) snprintf(g_dev_id, sizeof(g_dev_id), "%s", device_id);
    g_mq = mq;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = WTSN_I2C_SDA,
        .scl_io_num = WTSN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = { .clk_speed = WTSN_I2C_FREQ_HZ },
    };
    i2c_param_config(WTSN_I2C_PORT, &conf);
    i2c_driver_install(WTSN_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);

    bme280_init();

    /* PIR */
    gpio_config_t io = {0};
    io.pin_bit_mask = (1ULL << WTSN_PIR_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);

    /* ADC light */
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(WTSN_ADC_CH, ADC_ATTEN_DB_12);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100,
                             &g_adc_char);
    g_adc_cal = true;

    wtsn_sensor_actor_set_pin();
    actor_apply(0);

    /* micro:bit link on UART2 (TX GPIO17). */
    uart_config_t uc = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(WTSN_UART_PORT, &uc);
    uart_set_pin(WTSN_UART_PORT, WTSN_UART_TX, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(WTSN_UART_PORT, 1024, 2048, 0, NULL, 0);
    already_sent_mb_init = 1;
    microbit_send("{\"id\":\"" WTSN_SENSOR_DEV_ID "\",\"event\":\"init\"}");

    ESP_LOGI(TAG, "sensors ready (dev=%s): BME280 I2C, TEMT6000 ADC, HC-S501, actor",
             g_dev_id);
}

static int64_t g_last_tick = 0;

void wtsn_sensor_tick(void) {
    int64_t now = esp_timer_get_time() * 1000;
    if (now - g_last_tick < g_read_period_ns) return;
    g_last_tick = now;
    if (!g_mq) return;

    /* Only the device that actually has the sensors wired publishes telemetry.
     * Acccept both "esp32-01" (default) and a legacy "esp32-1" NVS id. */
    if (strcmp(g_dev_id, "esp32-01") != 0 && strcmp(g_dev_id, "esp32-1") != 0) return;

    float temp = -999, press = -999, hum = -999;
    bme280_read(&temp, &press, &hum);

    int light_mv = light_read_raw();
    int pir = gpio_get_level(WTSN_PIR_GPIO);

    /* PIR hold: keep reporting motion=1 for 5s after a trigger so the GUI has
     * time to notice even though the PIR pulse is short. */
    int64_t now_us = esp_timer_get_time();
    if (pir == 1) g_pir_latch = now_us;
    int pir_report = (pir == 1) ? 1 : ((now_us - g_pir_latch) < 5000000LL ? 1 : 0);

    char buf[512];
    int n = snprintf(buf, sizeof(buf) - 1,
            "{\"id\":\"%s\",\"sensors\":[", g_dev_id);
    size_t m = (size_t)n;

    int written = 0;
    char buf_temp[96], buf_press[96], buf_hum[96], buf_light[96], buf_pir[96];
    buf_temp[0] = buf_press[0] = buf_hum[0] = buf_light[0] = buf_pir[0] = 0;
    if (temp > -500) {
        snprintf(buf_temp, sizeof(buf_temp),
             "{\"sensor_id\":\"temp1\",\"type\":0,\"value\":%.1f,\"unit\":\"C\",\"healthy\":1}",
             temp);
        m += (size_t)snprintf(buf + m, sizeof(buf) - m, "%s%s", written++ ? "," : "", buf_temp);
    }
    if (press > -500) {
        snprintf(buf_press, sizeof(buf_press),
             "{\"sensor_id\":\"press1\",\"type\":1,\"value\":%.1f,\"unit\":\"hPa\",\"healthy\":1}",
             press);
        m += (size_t)snprintf(buf + m, sizeof(buf) - m, "%s%s", written++ ? "," : "", buf_press);
    }
    if (hum > 0) {
        snprintf(buf_hum, sizeof(buf_hum),
             "{\"sensor_id\":\"hum1\",\"type\":0,\"value\":%.1f,\"unit\":\"%%\",\"healthy\":1}",
             hum);
        m += (size_t)snprintf(buf + m, sizeof(buf) - m, "%s%s", written++ ? "," : "", buf_hum);
    }
    snprintf(buf_light, sizeof(buf_light),
         "{\"sensor_id\":\"light1\",\"type\":4,\"value\":%d,\"unit\":\"lx\",\"healthy\":1}", light_mv);
    m += (size_t)snprintf(buf + m, sizeof(buf) - m, "%s%s", written++ ? "," : "", buf_light);
    snprintf(buf_pir, sizeof(buf_pir),
         "{\"sensor_id\":\"pir1\",\"type\":4,\"value\":%d,\"unit\":\"\",\"healthy\":1}", pir_report);
    m += (size_t)snprintf(buf + m, sizeof(buf) - m, "%s%s", written++ ? "," : "", buf_pir);
    m += (size_t)snprintf(buf + m, sizeof(buf) - m,
         "%s{\"sensor_id\":\"actor_mode\",\"type\":4,\"value\":%d,\"unit\":\"\",\"healthy\":1}",
         written++ ? "," : "", g_actor_mode);
    /* stream cap */
    if (m > sizeof(buf) - 4) m = sizeof(buf) - 4;
    snprintf(buf + m, sizeof(buf) - m, "]}");

    wtsn_mqtt_publish(g_mq, "tsn/sensors", buf);
    microbit_send(buf);   /* mirror full JSON to the micro:bit display */

    /* cache for BLE panel */
    if (temp > -500) g_last_temp = temp;
    if (hum > 0) g_last_hum = hum;
    g_last_light = light_mv;
    g_last_pir = pir_report;

    /* Mirror the telemetry onto the OPC UA FX / C2C Field Exchange channel so the
     * values propagate over FXMQTT too (tsn/fx/<node>). */
    char fxtopic[48];
    snprintf(fxtopic, sizeof(fxtopic), "tsn/fx/%s", g_dev_id);
    wtsn_mqtt_publish(g_mq, fxtopic, buf);

    /* Per-sensor topics for a clean, per-reading view:
     *   tsn/sensors/<id>/<name>  e.g. tsn/sensors/esp32-1/temp
     */
    {
        char sub[96];
        snprintf(sub, sizeof(sub), "tsn/sensors/%s/temp", g_dev_id);
        wtsn_mqtt_publish(g_mq, sub, buf_temp);
        snprintf(sub, sizeof(sub), "tsn/sensors/%s/press", g_dev_id);
        wtsn_mqtt_publish(g_mq, sub, buf_press);
        snprintf(sub, sizeof(sub), "tsn/sensors/%s/hum", g_dev_id);
        wtsn_mqtt_publish(g_mq, sub, buf_hum);
        snprintf(sub, sizeof(sub), "tsn/sensors/%s/light", g_dev_id);
        wtsn_mqtt_publish(g_mq, sub, buf_light);
        snprintf(sub, sizeof(sub), "tsn/sensors/%s/pir", g_dev_id);
        wtsn_mqtt_publish(g_mq, sub, buf_pir);
    }

    if (pir != g_prev_pir) {
        char ev[96];
        snprintf(ev, sizeof(ev), "{\"id\":\"%s\",\"motion\":%d}", g_dev_id, pir);
        wtsn_mqtt_publish(g_mq, "tsn/sensors/event", ev);
        wtsn_mqtt_publish(g_mq, "tsn/fx/data", ev);
        g_prev_pir = pir;
    }
}

int wtsn_sensor_actor_set(int mode) {
    int prev = g_actor_mode;
    actor_apply(mode);
    return prev;
}

int wtsn_sensor_actor_get(void) {
    return g_actor_mode;
}

int wtsn_sensor_light(void) { return g_last_light; }
int wtsn_sensor_motion(void) { return g_last_pir; }
void wtsn_sensor_last(float *temp_c, float *hum_pct, int *light, int *pir, int *actor) {
    if (temp_c) *temp_c = g_last_temp;
    if (hum_pct) *hum_pct = g_last_hum;
    if (light) *light = g_last_light;
    if (pir) *pir = g_last_pir;
    if (actor) *actor = g_actor_mode;
}
