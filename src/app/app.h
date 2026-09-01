#ifndef WTSN_APP_H
#define WTSN_APP_H

#include "config_version/config_version_manager.h"
#include "common/common.h"
#include "db/db.h"
#include "device/device_manager.h"
#include "domain/domain_manager.h"
#include "fxmqtt/fxmqtt.h"
#include "mqtt/mqtt_client.h"
#include "mvc/event_bus.h"
#include "plugin/plugin_manager.h"
#include "qos/qos_manager.h"
#include "radio/wtsn_radio.h"
#include "sensors/sensor_manager.h"
#include "stream/tsn_manager.h"
#include "tas/tas_manager.h"
#include "timesync/timesync_manager.h"
#include "trace/trace.h"
#include "vlan/vlan_manager.h"

typedef struct {
    char db_path[WTSN_MAX_STR];
    char plugin_dir[WTSN_MAX_STR];
    char mqtt_host[128];
    int mqtt_port;
    bool headless;
} wtsn_app_config;

typedef struct wtsn_app {
    wtsn_db db;
    wtsn_event_bus *bus;
    wtsn_plugin_manager *plugins;
    wtsn_device_manager *devices;
    wtsn_qos_manager *qos;
    wtsn_vlan_manager *vlan;
    wtsn_timesync_manager *timesync;
    wtsn_tas_manager *tas;
    wtsn_sensor_manager *sensors;
    wtsn_tsn_manager *tsn;
    wtsn_mqtt_client *mqtt;
    wtsn_fxmqtt *fxmqtt;
    wtsn_trace *trace;
    wtsn_domain_manager *domains;
    wtsn_config_version_manager *cfgver;
    wtsn_app_config config;
} wtsn_app;

wtsn_error wtsn_app_init(wtsn_app *app, const wtsn_app_config *cfg);
void wtsn_app_shutdown(wtsn_app *app);
wtsn_error wtsn_app_run(wtsn_app *app);

#endif
