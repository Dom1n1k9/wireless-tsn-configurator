#include "simulator/protocol/sim_protocol.h"

#include "simulator/common/sim_log.h"
#include "simulator/common/sim_str.h"
#include "simulator/services/sim_services.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void dev_announce(sim_device *dev, void *ud);
static void publish_one(sim_device *dev, void *ud);
static void mqtt_detach(void *client);
static void publish(void *client, const char *topic, const char *payload);

struct sim_discovery {
    sim_simulator *sim;
    bool running;
};

struct sim_mqtt {
    sim_simulator *sim;
    void *client;
    char host[128];
    int port;
    bool connected;
};

struct sim_opcua {
    sim_simulator *sim;
    void *server;
    int base_port;
};

/* ---------------- discovery ---------------- */

sim_discovery *sim_discovery_create(sim_simulator *sim) {
    if (!sim) return NULL;
    sim_discovery *d = calloc(1, sizeof(sim_discovery));
    if (!d) return NULL;
    d->sim = sim;
    return d;
}

void sim_discovery_destroy(sim_discovery *d) {
    free(d);
}

sim_error sim_discovery_start(sim_discovery *d) {
    if (!d) return SIM_ERR_INVALID_ARG;
    d->running = true;
    return SIM_OK;
}

/* Publish a discovery announcement (multicast-style log + per-node line).
   In practice this would join a multicast group; here we print the payload
   that the configurator's discovery layer would consume. */
sim_error sim_discovery_announce(sim_discovery *d) {
    if (!d) return SIM_ERR_INVALID_ARG;
    sim_log(SIM_LOG_INFO, "discovery announce");
    sim_simulator_for_each(d->sim, dev_announce, NULL);
    return SIM_OK;
}

static void dev_announce(sim_device *dev, void *ud) {
    (void)ud;
    char sensors[256];
    sim_sensor_report(dev, sensors, sizeof(sensors));
    sim_log(SIM_LOG_INFO, "node %s kind=%s ip=%s fw=%s sensors={%s}",
            dev->id, sim_device_kind_str(dev->kind), dev->ip, dev->firmware,
            sensors);
}

/* ---------------- mqtt ---------------- */

sim_mqtt *sim_mqtt_create(sim_simulator *sim) {
    if (!sim) return NULL;
    sim_mqtt *m = calloc(1, sizeof(sim_mqtt));
    if (!m) return NULL;
    m->sim = sim;
    m->port = 1883;
    strcpy(m->host, "localhost");
    return m;
}

void sim_mqtt_destroy(sim_mqtt *m) {
    if (m && m->client) mqtt_detach(m);
    free(m);
}

sim_error sim_mqtt_connect(sim_mqtt *m, const char *host, int port) {
    if (!m || !host) return SIM_ERR_INVALID_ARG;
    m->connected = false;
    /* Real integration would use libmosquitto here. Keeping a thin abstraction
       so the simulator builds without hardwiring to one broker. */
    strcpy(m->host, host);
    m->port = port;
    m->connected = true;
    sim_log(SIM_LOG_INFO, "mqtt connected to %s:%d", host, port);
    return SIM_OK;
}

sim_error sim_mqtt_publish_all(sim_mqtt *m) {
    if (!m || !m->connected) return SIM_ERR_INVALID_ARG;
    sim_simulator_for_each(m->sim, publish_one, m);
    return SIM_OK;
}

static void publish_one(sim_device *dev, void *ud) {
    sim_mqtt *m = (sim_mqtt *)ud;
    char payload[512];
    char sensors[256];
    sim_sensor_report(dev, sensors, sizeof(sensors));
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"kind\":\"%s\",\"status\":\"%s\",\"firmware\":\"%s\","
             "\"qos\":{\"prio\":%d,\"class\":%d,\"bw_kbps\":%d,\"latency_ms\":%d},"
             "\"vlan\":%d,\"gm\":\"%s\",\"offset_ns\":%lld,\"cycle_ns\":%lld,\"gate\":%s,"
             "\"sensors\":{%s}}",
             dev->id, sim_device_kind_str(dev->kind),
             dev->status == SIM_DEVICE_ONLINE ? "online" : (dev->status == SIM_DEVICE_ERROR ? "error" : "offline"),
             dev->firmware, dev->qos_priority, dev->qos_traffic_class,
             dev->qos_bandwidth_kbps, dev->qos_latency_ms, dev->vlan_id,
             dev->timesync_grandmaster, (long long)dev->timesync_offset_ns,
             (long long)dev->cycle_time_ns, sim_gcl_gate_open(dev) ? "open" : "closed",
             sensors);
    publish(m, dev->mqtt_topic, payload);
}

static void mqtt_detach(void *client) {
    (void)client;
}

static void publish(void *client, const char *topic, const char *payload) {
    (void)client;
    sim_log(SIM_LOG_INFO, "  %s <- %s", topic, payload);
}

/* ---------------- opcua ---------------- */

sim_opcua *sim_opcua_create(sim_simulator *sim) {
    if (!sim) return NULL;
    sim_opcua *o = calloc(1, sizeof(sim_opcua));
    if (!o) return NULL;
    o->sim = sim;
    return o;
}

void sim_opcua_destroy(sim_opcua *o) {
    free(o);
}

sim_error sim_opcua_start(sim_opcua *o, int base_port) {
    if (!o) return SIM_ERR_INVALID_ARG;
    o->base_port = base_port;
    sim_log(SIM_LOG_INFO, "opc ua endpoints base port %d", base_port);
    return SIM_OK;
}

sim_error sim_opcua_update(sim_opcua *o) {
    (void)o;
    return SIM_OK;
}
