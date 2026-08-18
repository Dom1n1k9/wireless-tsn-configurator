#ifndef WTSN_GATEWAY_H
#define WTSN_GATEWAY_H

#include "common/common.h"
#include "mqtt/mqtt_client.h"
#include "opcua/opcua_server.h"

#define WTSN_GATEWAY_TOPIC_ALLOW "*"
#define WTSN_GATEWAY_TOPIC_PREFIX "tsn/node"

typedef struct wtsn_gateway wtsn_gateway;

wtsn_gateway *wtsn_gateway_create(wtsn_mqtt_client *mqtt, wtsn_opcua_server *opcua);
void wtsn_gateway_destroy(wtsn_gateway *gw);
wtsn_error wtsn_gateway_map_topic(wtsn_gateway *gw, const char *mqtt_topic, const char *opcua_path);
wtsn_error wtsn_gateway_start(wtsn_gateway *gw);

#endif
