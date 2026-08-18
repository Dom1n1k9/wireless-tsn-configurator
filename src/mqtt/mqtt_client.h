#ifndef WTSN_MQTT_CLIENT_H
#define WTSN_MQTT_CLIENT_H

#include "common/common.h"
#include "device/device.h"
#include "mvc/event_bus.h"

typedef struct wtsn_mqtt_client wtsn_mqtt_client;

typedef void (*wtsn_mqtt_message_cb)(const char *topic, const char *payload, size_t len, void *ud);

wtsn_mqtt_client *wtsn_mqtt_client_create(wtsn_event_bus *bus);
void wtsn_mqtt_client_destroy(wtsn_mqtt_client *c);

wtsn_error wtsn_mqtt_client_connect(wtsn_mqtt_client *c, const char *host, int port,
                                    const char *client_id, const char *username, const char *password);
void wtsn_mqtt_client_disconnect(wtsn_mqtt_client *c);
wtsn_error wtsn_mqtt_client_subscribe(wtsn_mqtt_client *c, const char *topic);
wtsn_error wtsn_mqtt_client_publish(wtsn_mqtt_client *c, const char *topic, const char *payload);
void wtsn_mqtt_client_set_message_cb(wtsn_mqtt_client *c, wtsn_mqtt_message_cb cb, void *ud);
void wtsn_mqtt_client_loop_start(wtsn_mqtt_client *c);
void wtsn_mqtt_client_loop_stop(wtsn_mqtt_client *c);

#endif
