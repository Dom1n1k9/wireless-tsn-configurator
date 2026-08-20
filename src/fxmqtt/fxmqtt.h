#ifndef WTSN_FXMQTT_H
#define WTSN_FXMQTT_H

#include "common/common.h"
#include "mqtt/mqtt_client.h"

#define WTSN_FXMQTT_TOPIC_FIELD "tsn/fx/field"
#define WTSN_FXMQTT_TOPIC_DATA "tsn/fx/data"
#define WTSN_FXMQTT_TOPIC_NODE "tsn/fx/node"

typedef enum {
    WTSN_FXMQTT_SERVER_NONE = 0,
    WTSN_FXMQTT_SERVER_PC,     /* server = configurator (this PC) */
    WTSN_FXMQTT_SERVER_NODE     /* server = selected device node */
} wtsn_fxmqtt_server_type;

typedef struct {
    char device_id[WTSN_MAX_STR];
    wtsn_fxmqtt_server_type server_type;
    char broker_host[WTSN_MAX_STR];
    int broker_port;
    bool started;
} wtsn_fxmqtt;

wtsn_fxmqtt *wtsn_fxmqtt_create(void);
void wtsn_fxmqtt_destroy(wtsn_fxmqtt *f);
wtsn_error wtsn_fxmqtt_configure(wtsn_fxmqtt *f, const wtsn_fxmqtt *cfg);
wtsn_error wtsn_fxmqtt_start(wtsn_fxmqtt *f, wtsn_mqtt_client *mqtt);
wtsn_error wtsn_fxmqtt_field_publish(wtsn_fxmqtt *f, const char *payload);
wtsn_error wtsn_fxmqtt_send_c2c(wtsn_fxmqtt *f, const char *topic, const char *payload);

#endif
