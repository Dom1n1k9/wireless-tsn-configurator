#include "simulator/core/sim_device.h"

#include <string.h>
#include <strings.h>

const char *sim_device_kind_str(sim_device_kind k) {
    switch (k) {
    case SIM_DEVICE_KIND_ESP32: return "ESP32";
    case SIM_DEVICE_KIND_RASPBERRY_PI: return "RaspberryPi";
    case SIM_DEVICE_KIND_STM32: return "STM32";
    case SIM_DEVICE_KIND_NXP: return "NXP";
    case SIM_DEVICE_KIND_LINUX: return "Linux";
    default: return "Generic";
    }
}

sim_device_kind sim_device_kind_parse(const char *s) {
    if (!s) return SIM_DEVICE_KIND_GENERIC;
    if (strcasecmp(s, "esp32") == 0) return SIM_DEVICE_KIND_ESP32;
    if (strcasecmp(s, "raspberry_pi") == 0 || strcasecmp(s, "raspberrypi") == 0 ||
        strcasecmp(s, "rpi") == 0 || strcasecmp(s, "raspberry pi") == 0)
        return SIM_DEVICE_KIND_RASPBERRY_PI;
    if (strcasecmp(s, "stm32") == 0) return SIM_DEVICE_KIND_STM32;
    if (strcasecmp(s, "nxp") == 0) return SIM_DEVICE_KIND_NXP;
    if (strcasecmp(s, "linux") == 0) return SIM_DEVICE_KIND_LINUX;
    return SIM_DEVICE_KIND_GENERIC;
}

const char *sim_sensor_type_str(sim_sensor_type t) {
    switch (t) {
    case SIM_SENSOR_TEMPERATURE: return "temperature";
    case SIM_SENSOR_PRESSURE: return "pressure";
    case SIM_SENSOR_IMU: return "imu";
    case SIM_SENSOR_DISTANCE: return "distance";
    case SIM_SENSOR_GPIO: return "gpio";
    default: return "unknown";
    }
}

sim_sensor_type sim_sensor_type_parse(const char *s) {
    if (!s) return SIM_SENSOR_GPIO;
    if (strcmp(s, "temperature") == 0) return SIM_SENSOR_TEMPERATURE;
    if (strcmp(s, "pressure") == 0) return SIM_SENSOR_PRESSURE;
    if (strcmp(s, "imu") == 0) return SIM_SENSOR_IMU;
    if (strcmp(s, "distance") == 0) return SIM_SENSOR_DISTANCE;
    return SIM_SENSOR_GPIO;
}
