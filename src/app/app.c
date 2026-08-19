#include "app/app.h"

#include "common/log.h"
#include "common/str_util.h"
#include "pubsub/pubsub_opcua.h"
#include "pubsub/pubsub_loopback.h"

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

    app->trace = wtsn_trace_create(app->bus);

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

    /* PubSub backend: real OPC UA PubSub if compiled in, else loopback (simulated).
       Both real and simulated endpoints are traced for the GUI. */
    wtsn_pubsub_backend pbackend;
    if (wtsn_pubsub_opcua_backend(&pbackend, wtsn_opcua_server_handle(app->opcua),
                                   (uint16_t)wtsn_opcua_server_ns(app->opcua)) != WTSN_OK) {
        wtsn_log(WTSN_LOG_WARN, "opc ua pubsub backend unavailable, no pubsub started");
        app->pubsub = NULL;
    } else {
        app->pubsub = calloc(1, sizeof(wtsn_pubsub));
        wtsn_pubsub_init(app->pubsub, &pbackend, pbackend.state);
        if (wtsn_pubsub_start(app->pubsub) != WTSN_OK) {
            /* fall back so the gateway still has a loopback pubsub to use */
            wtsn_pubsub_backend lb;
            wtsn_pubsub_loopback_backend(&lb, NULL);
            app->pubsub = calloc(1, sizeof(wtsn_pubsub));
            wtsn_pubsub_init(app->pubsub, &lb, NULL);
        }
        if (app->trace) wtsn_trace_add_config(app->trace, "pubsub",
            wtsn_pubsub_name(app->pubsub));
    }

    if (app->mqtt && app->pubsub) {
        app->gw_pubsub = wtsn_gateway_pubsub_create(app->mqtt, app->pubsub, app->trace);
        wtsn_gateway_pubsub_map_topic(app->gw_pubsub, "tsn/ns",
                                      "wtsnData");
        wtsn_gateway_pubsub_start(app->gw_pubsub);
    }

    if (app->mqtt) {
        app->gateway = wtsn_gateway_create(app->mqtt, app->opcua);
        wtsn_gateway_map_topic(app->gateway, "tsn/#", "/tsn/#");
        wtsn_gateway_start(app->gateway);
    }

    return WTSN_OK;
}

void wtsn_app_shutdown(wtsn_app *app) {
    if (!app) return;
    if (app->gw_pubsub) wtsn_gateway_pubsub_destroy(app->gw_pubsub);
    if (app->pubsub) free(app->pubsub);
    if (app->mqtt) wtsn_mqtt_client_destroy(app->mqtt);
    if (app->opcua) wtsn_opcua_server_destroy(app->opcua);
    if (app->gateway) wtsn_gateway_destroy(app->gateway);
    if (app->trace) wtsn_trace_destroy(app->trace);
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
