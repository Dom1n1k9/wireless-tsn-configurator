#include "wtsn_sensor.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/adc.h"
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
/* TEMT6000 analog light on ADC1 channel 0 (GPIO36) */
#define WTSN_ADC_CH ADC1_CHANNEL_0
/* HC-SR501 PIR motion on GPIO27 */
#ifndef WTSN_PIR_GPIO
#define WTSN_PIR_GPIO GPIO_NUM_27
#endif
/* Actor relay/switch on GPIO26 */
#ifndef WTSN_ACTOR_GPIO
#define WTSN_ACTOR_GPIO GPIO_NUM_26
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

static uint16_t calib_temp[3];  /* T1..T3 */
static int16_t  calib_press[9]; /* P1..P9 (P1 unsigned) */
static uint8_t   calib_hum[6];  /* H1..H2, H3..H6 signed */
static int8_t    calib_hum_2[2];

static int64_t g_read_period_ns = 2000000000LL; /* 2s */

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
        int o = 7 + 2 * i;
        calib_press[i] = (int16_t)((buf[o + 1] << 8) | buf[o]);
    }
    if (bme_reg_read(BME280_CALIB26_MSB, buf, 7) != ESP_OK) return -1;
    calib_hum[0] = buf[0];                                  /* H2 raw (16-bit) */
    calib_hum[1] = buf[1];
    calib_hum[2] = (int8_t)buf[2];                          /* H3 */
    calib_hum[3] = (int8_t)buf[3];                          /* H4 msb */
    calib_hum[4] = (int8_t)buf[4];                          /* H5 lsb */
    calib_hum[5] = buf[5];                                  /* H6 */
    /* real H2 signed 16-bit */
    int16_t h2 = (int16_t)((calib_hum[1] << 8) | calib_hum[0]);
    calib_hum_2[0] = (int8_t)(h2 & 0x7f);                /* low bits */
    calib_hum_2[1] = (int8_t)(h2 >> 8);                  /* sign bits */

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

    int32_t var1 = ((((adc_t >> 3) - ((int32_t)calib_temp[1] << 1)) *
                     ((int32_t)calib_temp[0])) >> 11);
    int32_t var2 = (((((adc_t >> 4) - ((int32_t)calib_temp[1])) *
                      ((adc_t >> 4) - ((int32_t)calib_temp[1]))) >> 12) *
                    ((int32_t)calib_temp[2])) >> 14;
    int32_t t_fine = var1 + var2;
    if (temp_c) *temp_c = (float)((t_fine * 5 + 128) >> 8) / 100.0f;

    if (press_hpa) {
        var1 = ((int32_t)t_fine) - 128000;
        var2 = var1 * var1;
        var2 = (var2 >> 8) * (int32_t)calib_press[5];
        var2 = var2 + ((var1 * (int32_t)calib_press[4]) << 17);
        var2 = var2 + (((int32_t)calib_press[4]) << 35);
        var1 = ((var1 * var1 * (int32_t)calib_press[2]) >> 8) +
               ((var1 * (int32_t)calib_press[1]) << 12);
        var1 = (((((int32_t)1 << 47) + var1)) * (int32_t)calib_press[0]) >> 33;
        if (var1) {
            int32_t p = 1048576 - adc_p;
            p = (int32_t)((((int64_t)p << 31) - var2) * 3125 / var1);
            var1 = (((int32_t)calib_press[8]) * ((p >> 13) * (p >> 13))) >> 25;
            var2 = (((int32_t)calib_press[7]) * p) >> 19;
            p = ((p + var1 + var2) >> 8) + (((int32_t)calib_press[3]) << 4);
            if (press_hpa) *press_hpa = (float)p / 100.0f;
        }
    }

    if (hum_pct && g_bme_ok) {
        int32_t v_x1_u32r = (t_fine - ((int32_t)76800));
        v_x1_u32r = (((((adc_h << 14) - (((int32_t)calib_hum[3]) << 20) -
                         (((int32_t)calib_hum_2[1]) * v_x1_u32r)) + ((int32_t)16384)) >> 15) *
                      (((((((v_x1_u32r * (int32_t)calib_hum[5]) >> 10) *
                          (((v_x1_u32r * (int32_t)calib_hum[2]) >> 11) +
                           ((int32_t)32768))) >> 10) + ((int32_t)2097152)) * (int32_t)calib_hum[0]) >> 20));
        v_x1_u32r = (v_x1_u32r - (((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7)) >> 4;
        if (v_x1_u32r < 0) v_x1_u32r = 0;
        if (v_x1_u32r > 419430400) v_x1_u32r = 419430400;
        *hum_pct = (float)(v_x1_u32r >> 12) / 1024.0f;
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
    /* TEMT6000: ~0.5 V ≈ 100 lux, ~2V ≈ 1000 lux (rough linear) */
    return (int)mv;
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

    ESP_LOGI(TAG, "sensors ready (dev=%s): BME280 I2C, TEMT6000 ADC, HC-SR501, actor",
             g_dev_id);
}

static int64_t g_last_tick = 0;

void wtsn_sensor_tick(void) {
    int64_t now = esp_timer_get_time() * 1000;
    if (now - g_last_tick < g_read_period_ns) return;
    g_last_tick = now;
    if (!g_mq) return;

    float temp = -999, press = -999, hum = -999;
    bme280_read(&temp, &press, &hum);

    int light_mv = light_read_raw();
    int pir = gpio_get_level(WTSN_PIR_GPIO);

    char buf[512];
    int n = snprintf(buf, sizeof(buf) - 1,
            "{\"id\":\"%s\",\"sensors\":[", g_dev_id);
    size_t m = (size_t)n;

    int written = 0;
    if (temp > -500) {
        m += (size_t)snprintf(buf + m, sizeof(buf) - m,
             "%s{\"sensor_id\":\"temp1\",\"type\":0,\"value\":%.1f,\"unit\":\"C\",\"healthy\":1}",
             written++ ? "," : "", temp);
    }
    if (press > -500) {
        m += (size_t)snprintf(buf + m, sizeof(buf) - m,
             "%s{\"sensor_id\":\"press1\",\"type\":1,\"value\":%.1f,\"unit\":\"hPa\",\"healthy\":1}",
             written++ ? "," : "", press);
    }
    if (hum > 0) {
        m += (size_t)snprintf(buf + m, sizeof(buf) - m,
             "%s{\"sensor_id\":\"hum1\",\"type\":0,\"value\":%.1f,\"unit\":\"%%\",\"healthy\":1}",
             written++ ? "," : "", hum);
    }
    m += (size_t)snprintf(buf + m, sizeof(buf) - m,
         "%s{\"sensor_id\":\"light1\",\"type\":4,\"value\":%d,\"unit\":\"mV\",\"healthy\":1}",
         written++ ? "," : "", light_mv);
    m += (size_t)snprintf(buf + m, sizeof(buf) - m,
         "%s{\"sensor_id\":\"pir1\",\"type\":4,\"value\":%d,\"unit\":\"\",\"healthy\":1}",
         written++ ? "," : "", pir);
    m += (size_t)snprintf(buf + m, sizeof(buf) - m,
         "%s{\"sensor_id\":\"actor_mode\",\"type\":4,\"value\":%d,\"unit\":\"\",\"healthy\":1}",
         written++ ? "," : "", g_actor_mode);
    /* stream cap */
    if (m > sizeof(buf) - 4) m = sizeof(buf) - 4;
    snprintf(buf + m, sizeof(buf) - m, "]}");

    wtsn_mqtt_publish(g_mq, "tsn/sensors", buf);

    if (pir != g_prev_pir) {
        char ev[96];
        snprintf(ev, sizeof(ev), "{\"id\":\"%s\",\"motion\":%d}", g_dev_id, pir);
        wtsn_mqtt_publish(g_mq, "tsn/sensors/event", ev);
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
