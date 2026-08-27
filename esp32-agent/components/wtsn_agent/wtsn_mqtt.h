#ifndef WTSN_MQTT_H
#define WTSN_MQTT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct wtsn_mqtt wtsn_mqtt;

/* on_command (topic, payload) is called with a normalized topic and payload
   whenever a command arrives. */
typedef void (*wtsn_cmd_cb)(const char *topic, const char *payload, void *ud);

typedef void (*wtsn_connected_cb)(const char *client_id, void *ud);

wtsn_mqtt *wtsn_mqtt_create(const char *host, int port, const char *client_id,
                             wtsn_cmd_cb cb, wtsn_connected_cb conn_cb, void *ud);
void wtsn_mqtt_start(wtsn_mqtt *m);
void wtsn_mqtt_publish(wtsn_mqtt *m, const char *topic, const char *payload);
void wtsn_mqtt_set_device_id(wtsn_mqtt *m, const char *id);

#endif
