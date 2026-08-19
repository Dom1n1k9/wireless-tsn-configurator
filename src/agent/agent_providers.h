#ifndef AGENT_PLATFORM_PROVIDERS_H
#define AGENT_PLATFORM_PROVIDERS_H

#include "agent/platform/agent_platform.h"
#include "mqtt/mqtt_client.h"

/* Linux / Raspberry Pi: native process executing TSN via iproute2/tc/ethtool */
agent_platform_ops agt_linux_ops(void);
void *agt_linux_state_create(wtsn_mqtt_client *mqtt);

/* Embedded platforms: ESP32 / STM32 / NXP - compile-safe stubs that log.
   Real implementation would drive ESP-IDF / Zephyr / vendor SDK TSN blocks. */
agent_platform_ops agt_embedded_ops(agent_platform p);
void *agt_embedded_state_create(void);
void agt_embedded_state_set(void *state, agent_platform p);

#endif
