#include "sensors/sensor.h"

#include <string.h>

const char *wtsn_sensor_type_str(wtsn_sensor_type t) {
    switch (t) {
    case WTSN_SENSOR_TEMPERATURE: return "temperature";
    case WTSN_SENSOR_PRESSURE: return "pressure";
    case WTSN_SENSOR_IMU: return "imu";
    case WTSN_SENSOR_DISTANCE: return "distance";
    case WTSN_SENSOR_GPIO: return "gpio";
    default: return "unknown";
    }
}

wtsn_sensor_type wtsn_sensor_type_parse(const char *s) {
    if (!s) return WTSN_SENSOR_TEMPERATURE;
    if (strcmp(s, "temperature") == 0) return WTSN_SENSOR_TEMPERATURE;
    if (strcmp(s, "pressure") == 0) return WTSN_SENSOR_PRESSURE;
    if (strcmp(s, "imu") == 0) return WTSN_SENSOR_IMU;
    if (strcmp(s, "distance") == 0) return WTSN_SENSOR_DISTANCE;
    if (strcmp(s, "gpio") == 0) return WTSN_SENSOR_GPIO;
    return WTSN_SENSOR_TEMPERATURE;
}
