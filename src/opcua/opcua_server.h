#ifndef WTSN_OPCUA_SERVER_H
#define WTSN_OPCUA_SERVER_H

#include "common/common.h"
#include "device/device.h"
#include "sensors/sensor.h"

#include <stdint.h>

typedef struct wtsn_opcua_server wtsn_opcua_server;

wtsn_opcua_server *wtsn_opcua_server_create(void);
void wtsn_opcua_server_destroy(wtsn_opcua_server *s);
wtsn_error wtsn_opcua_server_start(wtsn_opcua_server *s, uint16_t port);
wtsn_error wtsn_opcua_server_stop(wtsn_opcua_server *s);
wtsn_error wtsn_opcua_server_add_device(wtsn_opcua_server *s, const wtsn_device *dev);
wtsn_error wtsn_opcua_server_update_sensor(wtsn_opcua_server *s, const wtsn_sensor *sen);
void wtsn_opcua_server_process(wtsn_opcua_server *s);
wtsn_error wtsn_opcua_server_export_model(wtsn_opcua_server *s, const char *file);
uint16_t wtsn_opcua_server_port(wtsn_opcua_server *s);
void *wtsn_opcua_server_handle(wtsn_opcua_server *s);
int wtsn_opcua_server_ns(wtsn_opcua_server *s);

#endif
