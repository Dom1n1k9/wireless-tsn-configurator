#include "wtsn_sonar.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "sonar";

/* ---- pins (esp32-02 actor board) ---- */
#ifndef WTSN_SONAR_TRIG_GPIO
#define WTSN_SONAR_TRIG_GPIO GPIO_NUM_13
#endif
#ifndef WTSN_SONAR_ECHO_GPIO
#define WTSN_SONAR_ECHO_GPIO GPIO_NUM_12
#endif
#ifndef WTSN_SONAR_SERVO_GPIO
#define WTSN_SONAR_SERVO_GPIO GPIO_NUM_18
#endif

#define SONAR_ECHO_TIMEOUT_US  40000
#define SONAR_SWEEP_ANGLES     180          /* one step per degree */
#define SONAR_STEP_MS          35           /* time per angle step */
#define SERVO_FREQ_HZ          50           /* SG90: 50 Hz */
#define SERVO_MIN_US           1000         /* 0 deg (safe SG90 pulse) */
#define SERVO_MAX_US           2000         /* 180 deg (safe SG90 pulse) */
#define SERVO_DUTY_RES         LEDC_TIMER_14_BIT   /* 16384 */

static char g_dev_id[32] = "esp32-02";
static wtsn_mqtt *g_mq = NULL;

static int16_t g_sweep[SONAR_SWEEP_ANGLES];
static int g_sweep_len = 0;
static int32_t g_sweep_id = 0;
static bool g_sweep_ok = false;

static volatile bool g_trigger = false;
static volatile bool g_sweeping = false;
static volatile int  g_cur_angle = -1;

/* ---------------- servo (SG90, 50 Hz PWM) ---------------- */
/* Set pulse width in microseconds: 500 us = 0 deg, 2500 us = 180 deg. */
static void servo_set_us(uint16_t us) {
    uint32_t duty = ((uint32_t)us * SERVO_DUTY_RES) / 20000u;   /* us / 20000 * 2^duty */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void servo_set_angle(int deg) {
    if (deg < 0) deg = 0;
    if (deg > 180) deg = 180;
    uint16_t us = (uint16_t)(SERVO_MIN_US +
                             (deg * (SERVO_MAX_US - SERVO_MIN_US)) / 180);
    servo_set_us(us);
}

/* ---------------- HC-SR04 read (blocking echo) ---------------- */
static int sonar_read_cm(void) {
    gpio_set_level(WTSN_SONAR_TRIG_GPIO, 0);
    esp_rom_delay_us(5);
    gpio_set_level(WTSN_SONAR_TRIG_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(WTSN_SONAR_TRIG_GPIO, 0);

    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(WTSN_SONAR_ECHO_GPIO) == 0) {
        if (esp_timer_get_time() - t0 > SONAR_ECHO_TIMEOUT_US) return -1;
    }
    int64_t rise = esp_timer_get_time();
    while (gpio_get_level(WTSN_SONAR_ECHO_GPIO) == 1) {
        if (esp_timer_get_time() - t0 > SONAR_ECHO_TIMEOUT_US) {
            rise = -1;
            break;
        }
    }
    if (rise < 0) return -1;
    int64_t fall = esp_timer_get_time();
    float cm = (float)(fall - rise) / 58.0f;
    if (cm < 2.0f || cm > 400.0f) return -1;
    return (int)cm;
}

/* ---------------- publish sweep ---------------- */
static void publish_sweep(void) {
    if (!g_mq || g_sweep_len <= 0) return;
    /* {"id":"esp32-02","sonar":{"id":N,"sweep":[cm,..]},"ts":<unix>} */
    int buflen = 32 + g_sweep_len * 7;
    char *buf = malloc((size_t)buflen);
    if (!buf) return;
    int n = snprintf(buf, buflen, "{\"id\":\"%s\",\"sonar\":{\"id\":%ld,\"sweep\":[",
                     g_dev_id, (long)g_sweep_id);
    for (int i = 0; i < g_sweep_len; i++)
        n += snprintf(buf + n, buflen - n, "%s%d", i ? "," : "", g_sweep[i]);
    snprintf(buf + n, buflen - n, "]},\"ts\":%lld",
             (long long)esp_timer_get_time() / 1000000);
    wtsn_mqtt_publish(g_mq, "tsn/sonar", buf);
    free(buf);
}

/* ---------------- sweep task ---------------- */
static void sonar_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the servo settle after init */

    ESP_LOGI(TAG, "servo self-test: sweeping 0..180..0");
    for (int a = 0; a <= 180; a += 15) { servo_set_angle(a); vTaskDelay(pdMS_TO_TICKS(120)); }
    for (int a = 180; a >= 0; a -= 15) { servo_set_angle(a); vTaskDelay(pdMS_TO_TICKS(120)); }
    servo_set_angle(90);
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "servo self-test done (parked at 90)");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!g_trigger) continue;
        g_trigger = false;
        if (g_sweeping) continue;
        g_sweeping = true;
        ESP_LOGI(TAG, "sonar sweep start (motion)");

        for (int a = 0; a < SONAR_SWEEP_ANGLES; a++) {
            g_cur_angle = a;
            servo_set_angle(a);
            vTaskDelay(pdMS_TO_TICKS(SONAR_STEP_MS));  /* let servo move + echo settle */
            int cm = sonar_read_cm();
            g_sweep[a] = (int16_t)(cm < 0 ? -1 : cm);
        }
        servo_set_angle(90);           /* return to park position */
        g_sweep_len = SONAR_SWEEP_ANGLES;
        g_sweep_id++;
        g_sweep_ok = true;
        g_cur_angle = -1;
        g_sweeping = false;
        publish_sweep();
        ESP_LOGI(TAG, "sonar sweep done (%d angles)", g_sweep_len);
    }
}

/* ---------------- public API ---------------- */

void wtsn_sonar_init(const char *device_id, wtsn_mqtt *mq) {
    if (device_id) snprintf(g_dev_id, sizeof(g_dev_id), "%s", device_id);
    g_mq = mq;

    gpio_config_t io = {0};
    io.pin_bit_mask = (1ULL << WTSN_SONAR_TRIG_GPIO);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(WTSN_SONAR_TRIG_GPIO, 0);

    io.pin_bit_mask = (1ULL << WTSN_SONAR_ECHO_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);

    /* servo PWM channel */
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_DUTY_RES,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tc);
    ledc_channel_config_t ch = {
        .gpio_num = WTSN_SONAR_SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
    servo_set_angle(0);

    for (int i = 0; i < SONAR_SWEEP_ANGLES; i++) g_sweep[i] = -1;

    xTaskCreatePinnedToCore(sonar_task, "wtsn_sonar", 4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "sonar ready: TRIG=%d ECHO=%d SERVO=%d (SG90)",
             WTSN_SONAR_TRIG_GPIO, WTSN_SONAR_ECHO_GPIO, WTSN_SONAR_SERVO_GPIO);
}

void wtsn_sonar_trigger(void) { g_trigger = true; }

const int16_t *wtsn_sonar_map(int *n) {
    if (n) *n = g_sweep_len;
    return g_sweep_ok ? g_sweep : NULL;
}

void wtsn_sonar_motor(int dir, int duty_pct) {
    (void)duty_pct;
    if (dir == 1) servo_set_angle(0);       /* dummy, kept for API compat */
}

void wtsn_sonar_set_angle(int deg) {
    g_cur_angle = deg;
    servo_set_angle(deg);
    ESP_LOGI(TAG, "servo manual -> %d deg (pulse %u us)", deg,
             (unsigned)(SERVO_MIN_US + (deg * (SERVO_MAX_US - SERVO_MIN_US)) / 180));
}

int wtsn_sonar_active_angle(void) { return g_cur_angle; }
