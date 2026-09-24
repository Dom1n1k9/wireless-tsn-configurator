#ifndef WTSN_DISPLAY_H
#define WTSN_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "wtsn_mqtt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SSD1306 128x64 I2C OLED on the actor board (esp32-02) + 4 button inputs.
 *
 * Wiring (matches the module with SCL/SDA/VCC/GND + K1..K4):
 *   VCC  -> 3V3
 *   GND  -> GND
 *   SCL  -> GPIO22  (same I2C bus as the sensor add-on: SDA21/SCL22)
 *   SDA  -> GPIO21
 *   K1..K4 -> GPIOs (see WTSN_BTN_* below), button other side to GND
 *
 * The display renders node status / live telemetry and each button publishes
 * a labelled event on tsn/sensors/<id> plus a short "button.*" ack so the
 * GUI monitor shows what happened. Buttons are also aggregated into the 2 s
 * telemetry tick (btn1..btn4 sensors) so the GUI Sensors page shows them.
 */

/* ---- pin map (defaults; override with -D at build if needed) ---- */
#ifndef WTSN_SSD1306_SDA
#define WTSN_SSD1306_SDA GPIO_NUM_21
#endif
#ifndef WTSN_SSD1306_SCL
#define WTSN_SSD1306_SCL GPIO_NUM_22
#endif
#ifndef WTSN_BTN1_GPIO
#define WTSN_BTN1_GPIO GPIO_NUM_32
#endif
#ifndef WTSN_BTN2_GPIO
#define WTSN_BTN2_GPIO GPIO_NUM_33
#endif
#ifndef WTSN_BTN3_GPIO
#define WTSN_BTN3_GPIO GPIO_NUM_34
#endif
#ifndef WTSN_BTN4_GPIO
#define WTSN_BTN4_GPIO GPIO_NUM_35
#endif

/* ---- public API ---- */

/* Init the OLED (if present) and the four buttons. safe to call always;
 * if no SSD1306 answers on I2C the display part simply stays blank. */
void wtsn_display_init(const char *device_id, wtsn_mqtt *mq);

/* Set arbitrary 2-line status (label + value). */
void wtsn_display_status(const char *line1, const char *line2);

/* Per-period update: refresh the rendered text + aggregate button state. */
void wtsn_display_tick(void);

/* True when an SSD1306 was actually found on the bus. */
bool wtsn_display_present(void);

/* Last pressed button (1..4, 0 = none) since the last tick. */
int wtsn_display_btn_last(void);

/* Accessors so telemetry callers can report btn state within the sensor JSON. */
bool wtsn_display_btn(int n);            /* 1..4, staggered latch */
void wtsn_display_buttons(int *k1, int *k2, int *k3, int *k4);

#ifdef __cplusplus
}
#endif

#endif /* WTSN_DISPLAY_H */
