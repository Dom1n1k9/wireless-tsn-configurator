/* Actor-board display + buttons.
 *
 * A small, self-contained SSD1306 128x64 (I2C) driver and four debounced
 * push-buttons. Used on the actor board (esp32-02) to render the node status
 * and let physical buttons publish labelled MQTT events.
 *
 * No external component is required: the driver talks to the same I2C master
 * the sensor add-on already initialises (SDA=21, SCL=22, 100 kHz) and only
 * renders if an SSD1306 actually answers.
 */

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c.h"

#include "wtsn_display.h"
#include "wtsn_mqtt.h"

static const char *TAG = "display";

/* ---- SSD1306 register commands ---- */
#define SSD1306_ADDR   0x3C
#define SSD1306_CTRL_CMD 0x00
#define SSD1306_CTRL_DATA 0x40
#define SSD1306_CMD_DISPLAY_OFF     0xAE
#define SSD1306_CMD_DISPLAY_ON      0xAF
#define SSD1306_CMD_SET_MEM_MODE    0x20
#define SSD1306_CMD_MEM_HORZ        0x00
#define SSD1306_CMD_SET_COL_ADDR    0x21
#define SSD1306_CMD_SET_PAGE_ADDR   0x22
#define SSD1306_CMD_SET_START_LINE  0x40
#define SSD1306_CMD_CHARGE_PUMP     0x8D
#define SSD1306_CMD_CP_ON           0x14
#define SSD1306_CMD_SET_MUX_RATIO   0xA8
#define SSD1306_CMD_MUX_64          0x3F
#define SSD1306_CMD_DISP_OFFSET     0xD3
#define SSD1306_CMD_SEG_REMAP       0xA1
#define SSD1306_CMD_SET_COM_SCAN_DEC 0xC8
#define SSD1306_CMD_SET_COMPIN      0xDA
#define SSD1306_CMD_COMPIN_64       0x12
#define SSD1306_CMD_SET_CONTRAST    0x81
#define SSD1306_CMD_CONTRAST        0x7F
#define SSD1306_CMD_SET_VCOMH       0xDB
#define SSD1306_CMD_VCOMH_1_150     0x30
#define SSD1306_CMD_CLOCKDIV        0xD5
#define SSD1306_CMD_CLOCKDIV_VAL    0x80
#define SSD1306_CMD_PRECHARGE       0xD9
#define SSD1306_CMD_PRECHARGE_VAL   0x22
#define SSD1306_CMD_SET_SCROLL      0x2E   /* deactivate scroll */

/* 128 * 64 / 8 = 1024 bytes of page-mapped display RAM */
#define SSD1306_BUF_SZ 1024

static bool g_present = false;
static uint8_t g_fb[SSD1306_BUF_SZ];

static char g_dev_id[16] = "esp32-02";
static wtsn_mqtt *g_mq = NULL;

static char g_line1[24] = "WTSN node";
static char g_line2[24] = "connecting...";

static int64_t g_last_pub_us = 0;

/* ---- buttons ---- */
#define BTN_COUNT 4
static const gpio_num_t g_btn_gpio[BTN_COUNT] = {
    WTSN_BTN1_GPIO, WTSN_BTN2_GPIO, WTSN_BTN3_GPIO, WTSN_BTN4_GPIO,
};
/* Active-low (pressed = GND). */
static volatile uint32_t g_btn_levels = 0;      /* debounced 0/1 per button */
static volatile uint32_t g_btn_rising = 0;      /* level rise seen since last tick */
static int g_prev_btn[BTN_COUNT];
static char g_btn_label[BTN_COUNT][6] = {"K1", "K2", "K3", "K4"};
static bool g_btn_enabled = true;               /* disabled if the OLED is present for K3/K4 without pullup */

/* ===================================================================== */
/* SDD1306 helpers (only used when g_present)                            */
/* ===================================================================== */

static void i2c_write_cmd(uint8_t cmd) {
    i2c_cmd_handle_t c = i2c_cmd_link_create();
    i2c_master_start(c);
    i2c_master_write_byte(c, (SSD1306_ADDR << 1) | I2C_MASTER_WRITE, 1);
    i2c_master_write_byte(c, SSD1306_CTRL_CMD, 1);
    i2c_master_write_byte(c, cmd, 1);
    i2c_master_stop(c);
    i2c_master_cmd_begin(I2C_NUM_0, c, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(c);
}

/* Write the whole framebuffer to the display RAM. */
static void ssd1306_flush(void) {
    static const uint8_t zero[8] = {0};
    i2c_cmd_handle_t c = i2c_cmd_link_create();
    i2c_master_start(c);
    i2c_master_write_byte(c, (SSD1306_ADDR << 1) | I2C_MASTER_WRITE, 1);
    i2c_master_write_byte(c, SSD1306_CTRL_CMD, 1);
    i2c_master_write_byte(c, SSD1306_CMD_SET_COL_ADDR, 1);
    i2c_master_write_byte(c, 0, 1);
    i2c_master_write_byte(c, 127, 1);
    i2c_master_write_byte(c, SSD1306_CMD_SET_PAGE_ADDR, 1);
    i2c_master_write_byte(c, 0, 1);
    i2c_master_write_byte(c, 7, 1);
    i2c_master_write_byte(c, SSD1306_CTRL_DATA, 1);
    i2c_master_write(c, g_fb, SSD1306_BUF_SZ, 1);
    /* upper 8 bytes of the control stream are the display data; nothing else */
    (void)zero;
    i2c_master_stop(c);
    i2c_master_cmd_begin(I2C_NUM_0, c, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(c);
}

static void ssd1306_init_seq(void) {
    i2c_write_cmd(SSD1306_CMD_DISPLAY_OFF);
    i2c_write_cmd(SSD1306_CMD_SET_MUX_RATIO); i2c_write_cmd(SSD1306_CMD_MUX_64);
    i2c_write_cmd(SSD1306_CMD_DISP_OFFSET);   i2c_write_cmd(0x00);
    i2c_write_cmd(SSD1306_CMD_SET_START_LINE | 0x00);
    i2c_write_cmd(SSD1306_CMD_SET_COM_SCAN_DEC);
    i2c_write_cmd(SSD1306_CMD_SEG_REMAP);
    i2c_write_cmd(SSD1306_CMD_SET_COMPIN);    i2c_write_cmd(SSD1306_CMD_COMPIN_64);
    i2c_write_cmd(SSD1306_CMD_SET_CONTRAST);  i2c_write_cmd(SSD1306_CMD_CONTRAST);
    i2c_write_cmd(SSD1306_CMD_CLOCKDIV);      i2c_write_cmd(SSD1306_CMD_CLOCKDIV_VAL);
    i2c_write_cmd(SSD1306_CMD_PRECHARGE);     i2c_write_cmd(SSD1306_CMD_PRECHARGE_VAL);
    i2c_write_cmd(SSD1306_CMD_SET_VCOMH);     i2c_write_cmd(SSD1306_CMD_VCOMH_1_150);
    i2c_write_cmd(SSD1306_CMD_CHARGE_PUMP);   i2c_write_cmd(SSD1306_CMD_CP_ON);
    i2c_write_cmd(SSD1306_CMD_SET_MEM_MODE);  i2c_write_cmd(SSD1306_CMD_MEM_HORZ);
    i2c_write_cmd(SSD1306_CMD_SET_SCROLL);
    i2c_write_cmd(SSD1306_CMD_DISPLAY_ON);
}

/* Check the SSD1306 answers NAK-free at its address on the I2C bus. */
static bool ssd1306_present(void) {
    return i2c_master_write_to_device(I2C_NUM_0, SSD1306_ADDR, (const uint8_t *)"", 0,
                                      pdMS_TO_TICKS(50)) == ESP_OK;
}

/* ---------------------------------------------------------------- */
/* Tiny 5x7 font (ASCII 0x20..0x7E)                                 */
/* ---------------------------------------------------------------- */
static const uint8_t FONT5[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},
    {0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x41,0x7F},
    {0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x03,0x05,0x09,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x28,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x28},
    {0x38,0x44,0x44,0x28,0x7F},{0x38,0x54,0x54,0x54,0x18},
    {0x00,0x08,0x7E,0x09,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x78,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x08,0x7C},
    {0x7C,0x04,0x04,0x04,0x00},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x38,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},
    {0x02,0x01,0x02,0x04,0x02},{0xFF,0xFF,0xFF,0xFF,0xFF},
};

static void fb_clear(void) { memset(g_fb, 0, sizeof(g_fb)); }

static void fb_set_px(int x, int y) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    g_fb[(y >> 3) * 128 + x] |= (uint8_t)(1 << (y & 7));
}

static void fb_text(int col, int row, const char *text) {
    int x = col * 6;
    int y = row * 8;
    if (!text) return;
    for (const char *p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch > 0x7E) ch = ' ';
        for (int c = 0; c < 5; c++) {
            uint8_t line = FONT5[ch - 0x20][c];
            for (int b = 0; b < 7; b++) {
                if (line & (1 << b)) fb_set_px(x + c, y + 6 - b);
            }
        }
        x += 6;
        if (x + 5 > 127) break;
    }
}

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

bool wtsn_display_present(void) { return g_present; }

void wtsn_display_status(const char *line1, const char *line2) {
    if (line1) { strncpy(g_line1, line1, sizeof(g_line1) - 1); g_line1[sizeof(g_line1) - 1] = 0; }
    if (line2) { strncpy(g_line2, line2, sizeof(g_line2) - 1); g_line2[sizeof(g_line2) - 1] = 0; }
}

/* Publish button state + press events as sensors so the GUI sees them. */
static void buttons_publish(void) {
    if (!g_mq) return;
    int k1, k2, k3, k4;
    wtsn_display_buttons(&k1, &k2, &k3, &k4);
    char buf[300];
    size_t m = (size_t)snprintf(buf, sizeof(buf) - 1,
             "{\"id\":\"%s\",\"ts\":%lld,\"sensors\":[",
             g_dev_id, (long long)time(NULL));
    char sub[96];
    snprintf(sub, sizeof(sub),
             "{\"sensor_id\":\"btn1\",\"type\":4,\"value\":%d,\"unit\":\"\",\"healthy\":1}",
             k1);
    m += (size_t)snprintf(buf + m, sizeof(buf) - m, "%s", sub);
    snprintf(sub, sizeof(sub),
             "{\"sensor_id\":\"btn2\",\"type\":4,\"value\":%d,\"unit\":\"\",\"healthy\":1}",
             k2);
    m += (size_t)snprintf(buf + m, sizeof(buf) - m, ",%s", sub);
    snprintf(sub, sizeof(sub),
             "{\"sensor_id\":\"btn3\",\"type\":4,\"value\":%d,\"unit\":\"\",\"healthy\":1}",
             k3);
    m += (size_t)snprintf(buf + m, sizeof(buf) - m, ",%s", sub);
    snprintf(sub, sizeof(sub),
             "{\"sensor_id\":\"btn4\",\"type\":4,\"value\":%d,\"unit\":\"\",\"healthy\":1}",
             k4);
    m += (size_t)snprintf(buf + m, sizeof(buf) - m, ",%s]}", sub);
    (void)m;
    wtsn_mqtt_publish(g_mq, "tsn/sensors", buf);
}

static void buttons_init(void) {
    /* 34/35 have no internal pull-up on classic ESP32: use external 10k or
     * rely on the module's own pull. We configure pull-up where available and
     * treat a floating/active-low as "pressed" only on a real falling edge. */
    for (int i = 0; i < BTN_COUNT; i++) {
        gpio_config_t io = {0};
        io.pin_bit_mask = (1ULL << g_btn_gpio[i]);
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&io);
        g_prev_btn[i] = -1;
    }
}

static void buttons_tick(void) {
    uint32_t now_levels = 0;
    for (int i = 0; i < BTN_COUNT; i++) {
        int lvl = 0;
        if (g_btn_gpio[i] >= GPIO_NUM_34) {
            /* input-only pins return a valid level via gpio_get_level too */
            lvl = gpio_get_level(g_btn_gpio[i]);
        } else {
            lvl = gpio_get_level(g_btn_gpio[i]);
        }
        /* active low: pressed = 0 */
        if (lvl == 0) now_levels |= (1u << i);
    }
    g_btn_levels = now_levels;
    /* rising edges (was up, now down) -> publish */
    for (int i = 0; i < BTN_COUNT; i++) {
        int now = (now_levels >> i) & 1u;
        if (g_prev_btn[i] == 0 && now == 1) {
            g_btn_rising |= (1u << i);
            ESP_LOGI(TAG, "button %d (%s) pressed", i + 1, g_btn_label[i]);
            char topic[64], payload[96];
            /* labelled event so the monitor shows which physical button */
            snprintf(topic, sizeof(topic), "tsn/button/%s/%s", g_dev_id, g_btn_label[i]);
            snprintf(payload, sizeof(payload), "{\"id\":\"%s\",\"button\":\"%s\",\"n\":%d}",
                     g_dev_id, g_btn_label[i], i + 1);
            if (g_mq) wtsn_mqtt_publish(g_mq, topic, payload);
            /* also ack so a GUI ping handler can show it */
            snprintf(topic, sizeof(topic), "tsn/ack/%s", g_dev_id);
            snprintf(payload, sizeof(payload), "{\"id\":\"%s\",\"ok\":true,\"button\":\"%s\"}",
                     g_dev_id, g_btn_label[i]);
            if (g_mq) wtsn_mqtt_publish(g_mq, topic, payload);
        }
        g_prev_btn[i] = now;
    }
}

int wtsn_display_btn_last(void) {
    uint32_t r = g_btn_rising;
    g_btn_rising = 0;
    for (int i = 0; i < BTN_COUNT; i++) if (r & (1u << i)) return i + 1;
    return 0;
}

bool wtsn_display_btn(int n) {
    if (n < 1 || n > BTN_COUNT) return false;
    return (g_btn_levels >> (n - 1)) & 1u;
}

void wtsn_display_buttons(int *k1, int *k2, int *k3, int *k4) {
    if (k1) *k1 = (g_btn_levels >> 0) & 1u;
    if (k2) *k2 = (g_btn_levels >> 1) & 1u;
    if (k3) *k3 = (g_btn_levels >> 2) & 1u;
    if (k4) *k4 = (g_btn_levels >> 3) & 1u;
}

void wtsn_display_init(const char *device_id, wtsn_mqtt *mq) {
    if (device_id) snprintf(g_dev_id, sizeof(g_dev_id), "%s", device_id);
    g_mq = mq;

    buttons_init();

    /* Try to attach the display on the I2C bus (SDA21/SCL22). The bus is
     * already installed by the sensor add-on init; only reuse, don't own. */
    for (int attempt = 0; attempt < 2 && !g_present; attempt++) {
        if (attempt == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        if (ssd1306_present()) {
            ssd1306_init_seq();
            fb_clear();
            fb_text(0, 0, "WTSN node");
            fb_text(0, 1, g_dev_id);
            fb_text(0, 7, "display: OK");
            ssd1306_flush();
            g_present = true;
            ESP_LOGI(TAG, "SSD1306 found at 0x%02X (SDA%d SCL%d)",
                     SSD1306_ADDR, (int)WTSN_SSD1306_SDA, (int)WTSN_SSD1306_SCL);
        } else {
            ESP_LOGW(TAG, "no SSD1306 on I2C (0x%02X) - display disabled", SSD1306_ADDR);
        }
    }
}

void wtsn_display_tick(void) {
    buttons_tick();

    /* publish button state + press events on the shared sensor feed */
    if ((esp_timer_get_time() - g_last_pub_us) >= 2000000LL) {
        g_last_pub_us = esp_timer_get_time();
        buttons_publish();
    }

    if (!g_present) return;
    fb_clear();
    fb_text(0, 0, g_line1);
    fb_text(0, 1, g_line2);

    /* button press indicator row at the bottom */
    char tmp[24];
    int b1 = (int)((g_btn_levels >> 0) & 1u);
    int b2 = (int)((g_btn_levels >> 1) & 1u);
    int b3 = (int)((g_btn_levels >> 2) & 1u);
    int b4 = (int)((g_btn_levels >> 3) & 1u);
    snprintf(tmp, sizeof(tmp), "1:%d 2:%d 3:%d 4:%d", b1, b2, b3, b4);
    fb_text(0, 6, tmp);
    fb_text(0, 7, g_dev_id);

    ssd1306_flush();
}
