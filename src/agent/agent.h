#ifndef WTSN_AGENT_H
#define WTSN_AGENT_H

#include "common/common.h"
#include "agent/platform/agent_platform.h"
#include "mqtt/mqtt_client.h"

typedef struct {
    char device_id[WTSN_MAX_STR];
    char platform_str[WTSN_MAX_STR];
    agent_platform platform;
    agent_platform_ops ops;
    void *state;
    char mqtt_host[128];
    int mqtt_port;
    wtsn_mqtt_client *mqtt;
    /* internal handle to the command loop context */
    void *ctx;
} wtsn_agent;

wtsn_agent *wtsn_agent_create(const char *device_id, const char *platform_str,
                              const char *mqtt_host, int mqtt_port);
void wtsn_agent_destroy(wtsn_agent *a);
wtsn_error wtsn_agent_start(wtsn_agent *a);
wtsn_error wtsn_agent_handle_command(wtsn_agent *a, const char *command, const char *payload);
int wtsn_agent_run_loop(wtsn_agent *a);

#endif
