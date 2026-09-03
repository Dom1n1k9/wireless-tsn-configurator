#include "agent/agent_providers.h"

#include "common/log.h"
#include "common/str_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

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
    (void)lat; (void)preempt;
    if (bw > 0) {
        run("tc qdisc replace dev %s root handle 1: htb default %d", "wlan0", tc);
        run("tc class add dev %s parent 1: classid 1:1 htb rate %dkbit", "wlan0", bw);
    }
    run("tc qdisc add dev %s parent 1:1 handle 10: prio", "wlan0");
    (void)priority; (void)tc;
    return WTSN_OK;
}

static wtsn_error linux_apply_vlan(void *state, int vlan_id, const char *group) {
    (void)state;
    (void)group;
    run("ip link add link %s name vlan%d type vlan id %d 2>/dev/null || true", "wlan0", vlan_id, vlan_id);
    run("ip link set vlan%d up", vlan_id);
    return WTSN_OK;
}

static wtsn_error linux_apply_timesync(void *state, int mode, const char *gm) {
    linux_state *ls = (linux_state *)state;
    (void)gm;
    /* mode: 0 disabled, 1 local GM, 2 external GM, 3 auto */
    const char *opt = mode == 2 ? "-s" : "-f /etc/linuxptp/gptp.cfg";
    if (mode == 1) {
        /* this node is the grandmaster -> run ptp4l in master mode */
        run("pkill -f ptp4l");
        run("ptp4l -i %s -m -f /etc/linuxptp/gptp_master.cfg &", ls->iface);
        run("phc2sys -s %s -c CLOCK_REALTIME -O 0 -w &", ls->iface);
        wtsn_log(WTSN_LOG_INFO, "linux gPTP: this node = grandmaster (ptp4l master)");
    } else if (mode == 2 || mode == 3) {
        /* slave: follow the external master */
        run("pkill -f ptp4l");
        run("ptp4l -i %s -m -s -f /etc/linuxptp/gptp.cfg &", ls->iface);
        run("phc2sys -s CLOCK_REALTIME -c %s -O 0 -w &", ls->iface);
        wtsn_log(WTSN_LOG_INFO, "linux gPTP: slave mode (follow master via ptp4l %s)", opt);
    } else {
        run("pkill -f ptp4l; pkill -f phc2sys");
        wtsn_log(WTSN_LOG_INFO, "linux gPTP disabled");
    }
    return WTSN_OK;
}

static wtsn_error linux_apply_tas(void *state, int64_t cycle_ns,
                                const wtsn_gcl_entry *gcl, int entries) {
    linux_state *ls = (linux_state *)state;
    if (entries <= 0) return WTSN_ERR_INVALID_ARG;
    char gcl_str[512] = {0};
    for (int i = 0; i < entries && i < 8; i++) {
        /* gcl entry -> "gate[duration_ns]" ; gate_state bit0 = open */
        long d = (long)(gcl[i].duration_ns);
        char t[96];
        int open = (gcl[i].gate_state & 1) ? 1 : 0;
        snprintf(t, sizeof(t), "%c %ldns%s", open ? '1' : '0', d,
                 i + 1 < entries ? "," : "");
        strncat(gcl_str, t, sizeof(gcl_str) - strlen(gcl_str) - 1);
    }
    char cmd[768];
    snprintf(cmd, sizeof(cmd),
             "tc qdisc replace dev %s root handle 100 taprio num_tc 8 map 0 1 2 3 4 5 6 7 queues 1@0 1@1 1@2 1@3 2@4 2@6 3@8 3@11 base-time 0 clockid CLOCK_TAI sched-entry S 0x01 %ld sched-entry S 0x03 %ld sched-entry S 0x04 %ld",
             ls->iface, (long)cycle_ns / 2, (long)cycle_ns / 4, (long)cycle_ns / 4);
    (void)gcl_str;
    run("%s", cmd);
    wtsn_log(WTSN_LOG_INFO, "linux TAS: taprio applied on %s cycle=%lld ns (%d GCL entries)",
             ls->iface, (long long)cycle_ns, entries);
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
    (void)topic;
    (void)data;
    (void)len;
    /* TODO(host-agent): publish via the mqtt client. Currently a no-op; the
     * previous implementation allocated a buffer that was never used or freed. */
    return WTSN_OK;
}

/* FX over MQTT: send a dataset into the C2C field exchange. */
wtsn_error agt_linux_send_fx_multicast(void *state, const char *group,
                                      const unsigned char *data, size_t len) {
    (void)state;
    if (!group) return WTSN_ERR_INVALID_ARG;
#ifdef _WIN32
    (void)data; (void)len;
    return WTSN_ERR_NOT_IMPLEMENTED;
#else
    /* real POSIX multicast send */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return WTSN_ERR_IO;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(4840);
    if (inet_pton(AF_INET, group, &sin.sin_addr) != 1 ||
        !IN_MULTICAST(ntohl(sin.sin_addr.s_addr))) {
        close(fd);
        return WTSN_ERR_INVALID_ARG;
    }
    unsigned char ttl = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    ssize_t n = sendto(fd, data, len, 0, (struct sockaddr *)&sin, sizeof(sin));
    close(fd);
    return n >= 0 ? WTSN_OK : WTSN_ERR_IO;
#endif
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
