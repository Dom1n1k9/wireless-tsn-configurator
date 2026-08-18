#include "app/app.h"

#include "common/log.h"
#include "common/str_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static wtsn_error load_plugins(wtsn_app *app) {
    if (strlen(app->config.plugin_dir) == 0) return WTSN_OK;
    /* load any .so files present in the plugin directory */
    char path[WTSN_MAX_STR];
    snprintf(path, sizeof(path), "%s/plugin-mqtt-discovery.so", app->config.plugin_dir);
    wtsn_plugin_manager_load(app->plugins, path);
    snprintf(path, sizeof(path), "%s/plugin-opcua-discovery.so", app->config.plugin_dir);
    wtsn_plugin_manager_load(app->plugins, path);
    return WTSN_OK;
}

wtsn_error wtsn_app_init(wtsn_app *app, const wtsn_app_config *cfg) {
    if (!app || !cfg) return WTSN_ERR_INVALID_ARG;
    memset(app, 0, sizeof(*app));
    app->config = *cfg;

    wtsn_log_init(WTSN_LOG_INFO, NULL);

    wtsn_error e = wtsn_db_open(&app->db, cfg->db_path[0] ? cfg->db_path : "wtsn.db");
    if (e != WTSN_OK) return e;

    app->bus = wtsn_event_bus_create();
    if (!app->bus) { wtsn_db_close(&app->db); return WTSN_ERR_NO_MEMORY; }

    app->plugins = wtsn_plugin_manager_create();
    app->devices = wtsn_device_manager_create(&app->db, app->bus, app->plugins);
    app->qos = wtsn_qos_manager_create(&app->db, app->bus);
    app->vlan = wtsn_vlan_manager_create(&app->db, app->bus);
    app->timesync = wtsn_timesync_manager_create(&app->db, app->bus);
    app->tas = wtsn_tas_manager_create(&app->db, app->bus);
    app->sensors = wtsn_sensor_manager_create(&app->db, app->bus);

    if (!app->devices || !app->qos || !app->vlan || !app->timesync ||
        !app->tas || !app->sensors) return WTSN_ERR_NO_MEMORY;

    load_plugins(app);
    wtsn_device_manager_discover_once(app->devices);

    if (strlen(cfg->mqtt_host) > 0) {
        app->mqtt = wtsn_mqtt_client_create(app->bus);
        wtsn_mqtt_client_connect(app->mqtt, cfg->mqtt_host, cfg->mqtt_port,
                                 "wtsn-configurator", NULL, NULL);
        wtsn_mqtt_client_loop_start(app->mqtt);
    }

    app->opcua = wtsn_opcua_server_create();
    wtsn_opcua_server_start(app->opcua, app->config.opcua_port);

    if (app->mqtt) {
        app->gateway = wtsn_gateway_create(app->mqtt, app->opcua);
        wtsn_gateway_map_topic(app->gateway, "tsn/#", "/tsn/#");
        wtsn_gateway_start(app->gateway);
    }

    return WTSN_OK;
}

void wtsn_app_shutdown(wtsn_app *app) {
    if (!app) return;
    if (app->mqtt) wtsn_mqtt_client_destroy(app->mqtt);
    if (app->opcua) wtsn_opcua_server_destroy(app->opcua);
    if (app->gateway) wtsn_gateway_destroy(app->gateway);
    if (app->sensors) wtsn_sensor_manager_destroy(app->sensors);
    if (app->tas) wtsn_tas_manager_destroy(app->tas);
    if (app->timesync) wtsn_timesync_manager_destroy(app->timesync);
    if (app->vlan) wtsn_vlan_manager_destroy(app->vlan);
    if (app->qos) wtsn_qos_manager_destroy(app->qos);
    if (app->devices) wtsn_device_manager_destroy(app->devices);
    wtsn_plugin_manager_destroy(app->plugins);
    wtsn_event_bus_destroy(app->bus);
    wtsn_db_close(&app->db);
}

wtsn_error wtsn_app_run(wtsn_app *app) {
    if (!app) return WTSN_ERR_INVALID_ARG;
    if (app->config.headless) {
        wtsn_log(WTSN_LOG_INFO, "headless mode: running ops loop (ctrl-c to stop)");
        for (;;) sleep(1);
    }
    return WTSN_OK;
}
