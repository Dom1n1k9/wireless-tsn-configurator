#ifndef WTSN_TELEMETRY_H
#define WTSN_TELEMETRY_H

#include "common/common.h"
#include "db/db.h"
#include "device/device_manager.h"
#include "mqtt/mqtt_client.h"
#include "timesync/timesync_manager.h"
#include "trace/trace.h"

typedef struct wtsn_telemetry wtsn_telemetry;

wtsn_telemetry *wtsn_telemetry_create(wtsn_device_manager *devices,
                                       wtsn_timesync_manager *timesync,
                                       wtsn_trace *trace);
void wtsn_telemetry_destroy(wtsn_telemetry *t);

/* Wire the handler onto an MQTT client and subscribe to status/heartbeat topics. */
wtsn_error wtsn_telemetry_attach(wtsn_telemetry *t, wtsn_mqtt_client *mqtt);
void wtsn_telemetry_detach(wtsn_telemetry *t);

#endif
