#ifndef SIM_DEVICE_H
#define SIM_DEVICE_H

#include "simulator/common/sim_common.h"

#include <stdint.h>
#include <time.h>

typedef enum {
    SIM_DEVICE_KIND_ESP32 = 0,
    SIM_DEVICE_KIND_RASPBERRY_PI,
    SIM_DEVICE_KIND_STM32,
    SIM_DEVICE_KIND_NXP,
    SIM_DEVICE_KIND_LINUX,
    SIM_DEVICE_KIND_GENERIC
} sim_device_kind;

typedef enum {
    SIM_DEVICE_ONLINE = 0,
    SIM_DEVICE_OFFLINE,
    SIM_DEVICE_ERROR
} sim_device_status;

typedef enum {
    SIM_SENSOR_TEMPERATURE = 0,
    SIM_SENSOR_PRESSURE,
    SIM_SENSOR_IMU,
    SIM_SENSOR_DISTANCE,
    SIM_SENSOR_GPIO
} sim_sensor_type;

typedef struct {
    char sensor_id[SIM_MAX_STR];
    char name[SIM_MAX_STR];
    char unit[SIM_MAX_STR];
    sim_sensor_type type;
    double value;
    double min;
    double max;
    double step;
} sim_sensor;

typedef struct {
    unsigned char gate_state;
    int64_t duration_ns;
} sim_gcl_entry;

typedef struct {
    char id[SIM_MAX_STR];
    char name[SIM_MAX_STR];
    char ip[SIM_MAX_STR];
    char mac[SIM_MAX_STR];
    char firmware[SIM_MAX_STR];
    char model[SIM_MAX_STR];
    char mqtt_topic[SIM_MAX_STR];
    sim_device_kind kind;
    sim_device_status status;
    char tsn_features[SIM_MAX_TSN_FEATURES][32];
    int tsn_feature_count;
    char gcl_deploy_target[SIM_MAX_STR];
    int64_t cycle_time_ns;
    sim_gcl_entry gcl[SIM_MAX_GCL_ENTRIES];
    int gcl_count;
    int64_t gcl_state;
    sim_sensor sensors[SIM_MAX_SENSORS];
    int sensor_count;
    int qos_priority;
    int qos_traffic_class;
    int qos_bandwidth_kbps;
    int qos_latency_ms;
    int vlan_id;
    char vlan_group[SIM_MAX_STR];
    int timesync_mode;
    char timesync_grandmaster[SIM_MAX_STR];
    char timesync_protocol[SIM_MAX_STR];
    int64_t timesync_offset_ns;
    bool services_qos;
    bool services_vlan;
    bool services_timesync;
    bool services_tas;
    bool services_sensors;
} sim_device;

const char *sim_device_kind_str(sim_device_kind k);
sim_device_kind sim_device_kind_parse(const char *s);
const char *sim_sensor_type_str(sim_sensor_type t);
sim_sensor_type sim_sensor_type_parse(const char *s);

#endif
