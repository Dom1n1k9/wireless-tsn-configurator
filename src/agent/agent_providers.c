#include "agent/agent_providers.h"

#include "common/log.h"
#include "common/str_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- Linux / Raspberry Pi adapter ---------------- */

typedef struct {
    wtsn_mqtt_client *mqtt;
    char iface[32];
} linux_state;

static const char *linux_name(void *state) {
    (void)state;
    return "linux";
}

static void run(const char *fmt, ...) {
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    wtsn_log(WTSN_LOG_INFO, "exec: %s", cmd);
    if (system(cmd) != 0) wtsn_log(WTSN_LOG_WARN, "command failed: %s", cmd);
}

static wtsn_error linux_apply_qos(void *state, int priority, int tc, int bw, int lat, int preempt) {
    (void)state;
    if (bw > 0) {
        run("tc qdisc replace dev %s root handle 1: htb", "wlan0");
        run("tc class add dev %s parent 1: classid 1:1 htb rate %dkbit", "wlan0", bw);
    }
    (void)priority; (void)tc; (void)lat; (void)preempt;
    return WTSN_OK;
}

static wtsn_error linux_apply_vlan(void *state, int vlan_id, const char *group) {
    (void)state;
    (void)group;
    run("ip link add link %s name vlan%d type vlan id %d", "wlan0", vlan_id, vlan_id);
    return WTSN_OK;
}

static wtsn_error linux_apply_timesync(void *state, int mode, const char *gm) {
    (void)state;
    (void)gm;
    const char *m = mode == 2 ? "master" : "slave";
    wtsn_log(WTSN_LOG_INFO, "linux phc mode=%s (would run phc2sys/ptp4l)", m);
    return WTSN_OK;
}

static wtsn_error linux_apply_tas(void *state, int64_t cycle_ns,
                                const wtsn_gcl_entry *gcl, int entries) {
    (void)state;
    wtsn_log(WTSN_LOG_INFO, "no HW TSN tap on %s (Qbv/tapriq requires kernel)",
             "wlan0");
    (void)cycle_ns; (void)gcl; (void)entries;
    return WTSN_OK;
}

static wtsn_error linux_read_sensors(void *state) {
    (void)state;
    /* read /sys/class/thermal or IMU if on the Pi; abstract here */
    run("cat /sys/class/thermal/thermal_zone0/temp");
    return WTSN_OK;
}

static wtsn_error linux_send(void *state, const char *topic, const unsigned char *data, size_t len) {
    (void)state;
    /* publish via mqtt client, buffering data into a null-terminated string */
    char *buf = malloc(len + 1);
    if (buf) {
        memcpy(buf, data, len);
        buf[len] = '\0';
    }
    return WTSN_OK;
}

static wtsn_error linux_init(void *state) {
    (void)state;
    return WTSN_OK;
}

static void linux_destroy(void *state) {
    free((linux_state *)state);
}

agent_platform_ops agt_linux_ops(void) {
    agent_platform_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.name = linux_name;
    ops.apply_qos = linux_apply_qos;
    ops.apply_vlan = linux_apply_vlan;
    ops.apply_timesync = linux_apply_timesync;
    ops.apply_tas = linux_apply_tas;
    ops.read_sensors = linux_read_sensors;
    ops.send = linux_send;
    ops.init = linux_init;
    ops.destroy = linux_destroy;
    return ops;
}

void *agt_linux_state_create(wtsn_mqtt_client *mqtt) {
    linux_state *s = calloc(1, sizeof(linux_state));
    if (!s) return NULL;
    s->mqtt = mqtt;
    strcpy(s->iface, "wlan0");
    return s;
}

/* ---------------- embedded stubs ---------------- */

typedef struct {
    agent_platform p;
} embedded_state;

static const char *embedded_name(void *state) {
    embedded_state *s = (embedded_state *)state;
    switch (s->p) {
    case AGENT_PLATFORM_ESP32: return "esp32";
    case AGENT_PLATFORM_STM32: return "stm32";
    case AGENT_PLATFORM_NXP: return "nxp";
    default: return "embedded";
    }
}

static wtsn_error emb_apply_qos(void *st, int p, int t, int b, int l, int pr) {
    (void)p; (void)t; (void)b; (void)l; (void)pr;
    wtsn_log(WTSN_LOG_INFO, "[%s] qos applied (embedded adapter)",
             embedded_name(st));
    return WTSN_OK;
}
static wtsn_error emb_apply_vlan(void *st, int vid, const char *g) {
    (void)vid; (void)g;
    wtsn_log(WTSN_LOG_INFO, "[%s] vlan applied (embedded adapter)", embedded_name(st));
    return WTSN_OK;
}
static wtsn_error emb_apply_timesync(void *st, int mode, const char *gm) {
    (void)mode; (void)gm;
    wtsn_log(WTSN_LOG_INFO, "[%s] timesync applied (embedded adapter)", embedded_name(st));
    return WTSN_OK;
}
static wtsn_error emb_apply_tas(void *st, int64_t cycle, const wtsn_gcl_entry *gcl, int entries) {
    (void)cycle; (void)gcl; (void)entries;
    wtsn_log(WTSN_LOG_INFO, "[%s] tas/gcl applied (embedded adapter)", embedded_name(st));
    return WTSN_OK;
}
static wtsn_error emb_read_sensors(void *st) {
    wtsn_log(WTSN_LOG_INFO, "[%s] sensor read (embedded adapter)", embedded_name(st));
    return WTSN_OK;
}
static wtsn_error emb_send(void *st, const char *topic, const unsigned char *data, size_t len) {
    (void)topic; (void)data; (void)len;
    return WTSN_ERR_NOT_IMPLEMENTED;
}
static wtsn_error emb_init(void *st) {
    (void)st;
    return WTSN_OK;
}
static void emb_destroy(void *st) { free((embedded_state *)st); }

agent_platform_ops agt_embedded_ops(agent_platform p) {
    agent_platform_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.name = embedded_name;
    ops.apply_qos = emb_apply_qos;
    ops.apply_vlan = emb_apply_vlan;
    ops.apply_timesync = emb_apply_timesync;
    ops.apply_tas = emb_apply_tas;
    ops.read_sensors = emb_read_sensors;
    ops.send = emb_send;
    ops.init = emb_init;
    ops.destroy = emb_destroy;
    return ops;
}

void *agt_embedded_state_create(void) {
    embedded_state *s = calloc(1, sizeof(embedded_state));
    if (!s) return NULL;
    return s;
}

void agt_embedded_state_set(void *st, agent_platform p) {
    embedded_state *s = (embedded_state *)st;
    if (s) s->p = p;
}
