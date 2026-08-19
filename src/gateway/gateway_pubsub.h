#ifndef WTSN_GATEWAY_PUBSUB_H
#define WTSN_GATEWAY_PUBSUB_H

#include "common/common.h"
#include "mqtt/mqtt_client.h"
#include "pubsub/pubsub.h"
#include "trace/trace.h"

typedef struct wtsn_gateway_pubsub wtsn_gateway_pubsub;

wtsn_gateway_pubsub *wtsn_gateway_pubsub_create(wtsn_mqtt_client *mqtt, wtsn_pubsub *pubsub, wtsn_trace *trace);
void wtsn_gateway_pubsub_destroy(wtsn_gateway_pubsub *gw);
wtsn_error wtsn_gateway_pubsub_map_topic(wtsn_gateway_pubsub *gw, const char *mqtt_topic, const char *pubsub_dataset);
wtsn_error wtsn_gateway_pubsub_start(wtsn_gateway_pubsub *gw);

#endif
