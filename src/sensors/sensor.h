#ifndef WTSN_SENSOR_H
#define WTSN_SENSOR_H

#include "common/common.h"

#include <time.h>

typedef enum {
    WTSN_SENSOR_TEMPERATURE = 0,
    WTSN_SENSOR_PRESSURE,
    WTSN_SENSOR_IMU,
    WTSN_SENSOR_DISTANCE,
    WTSN_SENSOR_GPIO
} wtsn_sensor_type;

typedef struct {
    char device_id[WTSN_MAX_STR];
    char sensor_id[WTSN_MAX_STR];
    wtsn_sensor_type type;
    char name[WTSN_MAX_STR];
    double value;
    char unit[32];
    bool healthy;
    time_t last_update;
} wtsn_sensor;

const char *wtsn_sensor_type_str(wtsn_sensor_type t);
wtsn_sensor_type wtsn_sensor_type_parse(const char *s);

#endif
