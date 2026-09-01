#include "app/app.h"

#include "common/log.h"
#include "common/str_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static wtsn_error load_plugins(wtsn_app *app) {
    if (strlen(app->config.plugin_dir) == 0) return WTSN_OK;
    char path[WTSN_MAX_STR];
    snprintf(path, sizeof(path), "%s/plugin-mqtt-discovery.so", app->config.plugin_dir);
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

    app->trace = wtsn_trace_create_persistent(app->bus, &app->db, 20000);

    app->plugins = wtsn_plugin_manager_create();
    app->devices = wtsn_device_manager_create(&app->db, app->bus, app->plugins);
    app->domains = wtsn_domain_manager_create(&app->db, app->bus);
    app->qos = wtsn_qos_manager_create(&app->db, app->bus);
    app->vlan = wtsn_vlan_manager_create(&app->db, app->bus);
    app->timesync = wtsn_timesync_manager_create(&app->db, app->bus);
    app->tas = wtsn_tas_manager_create(&app->db, app->bus);
    app->sensors = wtsn_sensor_manager_create(&app->db, app->bus);
    app->cfgver = wtsn_cfg_ver_manager_create(&app->db, app->bus);

    wtsn_tsn_manager_config tcfg;
    memset(&tcfg, 0, sizeof(tcfg));
    tcfg.db = &app->db;
    tcfg.bus = app->bus;
    tcfg.mqtt = NULL;   /* assigned below once the MQTT client exists */
    app->tsn = wtsn_tsn_manager_create(&tcfg);

    if (!app->devices || !app->qos || !app->vlan || !app->timesync ||
        !app->tas || !app->sensors || !app->tsn || !app->domains || !app->cfgver)
        return WTSN_ERR_NO_MEMORY;

    load_plugins(app);
    wtsn_device_manager_discover_once(app->devices);

    /* OPC UA FX over MQTT: the single communication channel (PubSub, C2C). */
    app->mqtt = NULL;
    app->fxmqtt = wtsn_fxmqtt_create();
    if (!app->fxmqtt) return WTSN_ERR_NO_MEMORY;

    if (strlen(cfg->mqtt_host) > 0) {
        app->mqtt = wtsn_mqtt_client_create(app->bus);
        wtsn_mqtt_client_connect(app->mqtt, cfg->mqtt_host, cfg->mqtt_port,
                                 "wtsn-configurator", NULL, NULL);
        wtsn_mqtt_client_loop_start(app->mqtt);
        wtsn_tsn_manager_set_mqtt(app->tsn, app->mqtt);
        app->telemetry = wtsn_telemetry_create(app->devices, app->timesync, app->trace);
        wtsn_telemetry_attach(app->telemetry, app->mqtt);
        wtsn_fxmqtt          *fcfg = app->fxmqtt;
        fcfg->broker_port = cfg->mqtt_port;
        wtsn_strlcpy(fcfg->broker_host, cfg->mqtt_host, sizeof(fcfg->broker_host));
        wtsn_fxmqtt_configure(app->fxmqtt, fcfg);
        wtsn_fxmqtt_start(app->fxmqtt, app->mqtt);
        if (app->trace) wtsn_trace_add_config(app->trace, "fxmqtt",
                "OPC UA FX over MQTT started");
        if (app->trace) wtsn_trace_add_config(app->trace, "telemetry",
                "Monitoring tsn/status/# and tsn/telemetry/# subscribed");
    }

    return WTSN_OK;
}

void wtsn_app_shutdown(wtsn_app *app) {
    if (!app) return;
    if (app->fxmqtt) wtsn_fxmqtt_destroy(app->fxmqtt);
    if (app->telemetry) wtsn_telemetry_destroy(app->telemetry);
    if (app->mqtt) wtsn_mqtt_client_destroy(app->mqtt);
    if (app->trace) wtsn_trace_destroy(app->trace);
    if (app->cfgver) wtsn_cfg_ver_manager_destroy(app->cfgver);
    if (app->sensors) wtsn_sensor_manager_destroy(app->sensors);
    if (app->tsn) wtsn_tsn_manager_destroy(app->tsn);
    if (app->tas) wtsn_tas_manager_destroy(app->tas);
    if (app->timesync) wtsn_timesync_manager_destroy(app->timesync);
    if (app->vlan) wtsn_vlan_manager_destroy(app->vlan);
    if (app->qos) wtsn_qos_manager_destroy(app->qos);
    if (app->domains) wtsn_domain_manager_destroy(app->domains);
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
