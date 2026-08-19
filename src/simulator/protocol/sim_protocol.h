#ifndef SIM_PROTOCOL_H
#define SIM_PROTOCOL_H

#include "simulator/core/sim_simulator.h"

typedef struct sim_discovery sim_discovery;
typedef struct sim_mqtt sim_mqtt;
typedef struct sim_opcua sim_opcua;

/* Discovery: advertises simulated nodes so the configurator can discover them. */
sim_discovery *sim_discovery_create(sim_simulator *sim);
void sim_discovery_destroy(sim_discovery *d);
sim_error sim_discovery_start(sim_discovery *d);
sim_error sim_discovery_announce(sim_discovery *d);

/* MQTT: publishes each node's state to a broker. */
sim_mqtt *sim_mqtt_create(sim_simulator *sim);
void sim_mqtt_destroy(sim_mqtt *m);
sim_error sim_mqtt_connect(sim_mqtt *m, const char *host, int port);
sim_error sim_mqtt_publish_all(sim_mqtt *m);

/* OPC UA FX / wireless multicast: all simulated nodes join a shared multicast
   group (UA-DP transport, default 239.255.0.1:4840) and a publisher spreads a
   dataset to every member - simulating the real FX behaviour, and each send is
   logged so the configurator's trace/monitoring can capture it. */
typedef void (*sim_mcast_cb)(const char *node_id, const char *group,
                              const char *dataset, void *ud);
sim_error sim_mqtt_publish_fx_multicast(sim_mqtt *m, const char *node_id,
                                        const char *dataset);
void sim_mqtt_set_fx_multicast_cb(sim_mqtt *m, sim_mcast_cb cb, void *ud);

/* OPC UA: hosts each node as an OPC UA server endpoint. */
sim_opcua *sim_opcua_create(sim_simulator *sim);
void sim_opcua_destroy(sim_opcua *o);
sim_error sim_opcua_start(sim_opcua *o, int base_port);
sim_error sim_opcua_update(sim_opcua *o);

#endif
