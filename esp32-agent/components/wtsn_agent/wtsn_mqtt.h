#ifndef WTSN_MQTT_H
#define WTSN_MQTT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct wtsn_mqtt wtsn_mqtt;

/* on_command (topic, payload) is called with a normalized topic and payload
   whenever a command arrives. */
typedef void (*wtsn_cmd_cb)(const char *topic, const char *payload, void *ud);

wtsn_mqtt *wtsn_mqtt_create(const char *host, int port, const char *client_id,
                             wtsn_cmd_cb cb, void *ud);
void wtsn_mqtt_start(wtsn_mqtt *m);
void wtsn_mqtt_publish(wtsn_mqtt *m, const char *topic, const char *payload);

#endif
