#ifndef WTSN_SONAR_H
#define WTSN_SONAR_H

#include <stdint.h>
#include <stdbool.h>

#include "wtsn_mqtt.h"

/* Panning sonar (HC-SR04 on a 9g micro servo, SG90) attached to the actor board
 * (esp32-02). A motion event (PIR / WiFi Vision via tsn/fx/data +
 * tsn/sensors/event) arms a sweep: the servo rotates the sensor while each step
 * fires a HC-SR04 echo; range + angle are published over MQTT so the web GUI
 * can draw a sonar/radar map.
 *
 * Pins (esp32-02 board):
 *   TRIG = GPIO13, ECHO = GPIO12 (HC-SR04)
 *   SERVO = GPIO16 (SG90 signal, 50 Hz PWM, 0.5-2.5 ms = 0-180 deg)
 */

/* Init sonar + servo. device_id + mq are used for MQTT publish. */
void wtsn_sonar_init(const char *device_id, wtsn_mqtt *mq);

/* Trigger a sweep. Called from the motion handler when esp32-01 reports motion.
 * Safe to call from any task; debounced internally. */
void wtsn_sonar_trigger(void);

/* Latest completed sweep, index = angle (deg) -> range in cm (-1 = none). */
const int16_t *wtsn_sonar_map(int *n);

/* Manual servo control helper (0=off, 1=angle sweep). */
void wtsn_sonar_motor(int dir, int duty_pct);

/* Set servo to a fixed angle [0..180]. Used for manual testing. */
void wtsn_sonar_set_angle(int deg);

/* Current sweep in progress (angle), -1 when idle. */
int wtsn_sonar_active_angle(void);

#endif
