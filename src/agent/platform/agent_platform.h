#ifndef AGENT_PLATFORM_H
#define AGENT_PLATFORM_H

#include "common/common.h"
#include "tas/gcl.h"

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    AGENT_PLATFORM_LINUX = 0,
    AGENT_PLATFORM_RASPBERRY_PI,
    AGENT_PLATFORM_ESP32,
    AGENT_PLATFORM_STM32,
    AGENT_PLATFORM_NXP
} agent_platform;

typedef struct {
    void *state;
    const char *(*name)(void *state);
    wtsn_error (*apply_qos)(void *state, int priority, int tc, int bw_kbps, int lat_ms, int preemption);
    wtsn_error (*apply_vlan)(void *state, int vlan_id, const char *group);
    wtsn_error (*apply_timesync)(void *state, int mode, const char *gm);
    wtsn_error (*apply_tas)(void *state, int64_t cycle_ns, const wtsn_gcl_entry *gcl, int entries);
    wtsn_error (*read_sensors)(void *state);
    wtsn_error (*send)(void *state, const char *topic, const unsigned char *data, size_t len);
    wtsn_error (*init)(void *state);
    void (*destroy)(void *state);
} agent_platform_ops;

agent_platform agt_platform_from_string(const char *s);
void agt_platform_default_ops(agent_platform_ops *ops, agent_platform p);

#endif
