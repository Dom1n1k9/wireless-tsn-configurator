#ifndef WTSN_SENSOR_H
#define WTSN_SENSOR_H

#include <stdint.h>
#include <stdbool.h>

#include "wtsn_mqtt.h"

/* Sensor types matching the webgui schema (type column). */
enum {
    WTSN_SENSOR_TEMP   = 0,
    WTSN_SENSOR_PRESS  = 1,
    WTSN_SENSOR_IMU    = 2,
    WTSN_SENSOR_DIST   = 3,
    WTSN_SENSOR_GPIO   = 4,
};

/* 7-mode timer switch (actor). Mode meanings:
 *  0 off, 1 manual-on, 2 always-on, 3 timed-once, 4 interval,
 *  5 delayed-start, 6 auto/cyclic */
#define TIMER_SWITCH_MODES 7

/* Init sensors + actor on this node. device_id is used on the MQTT payloads. */
void wtsn_sensor_init(const char *device_id, wtsn_mqtt *mq);

/* Periodic scan: read analog light, PIR motion and BME280, publish telemetry. */
void wtsn_sensor_tick(void);

/* Actor: set the output switch. mode 0-6, returns previous mode. */
int wtsn_sensor_actor_set(int mode);
int wtsn_sensor_actor_get(void);
void wtsn_sensor_actor_set_pin(void);

/* Value accessors for the BLE micro:bit panel. Returns 0 on missing data. */
int  wtsn_sensor_light(void);
int  wtsn_sensor_motion(void);
void wtsn_sensor_last(float *temp_c, float *hum_pct, int *light, int *pir, int *actor);

#endif
