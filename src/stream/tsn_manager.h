#ifndef WTSN_TSN_MANAGER_H
#define WTSN_TSN_MANAGER_H

#include "db/db.h"
#include "mqtt/mqtt_client.h"
#include "mvc/event_bus.h"
#include "stream/stream.h"

typedef struct wtsn_tsn_manager wtsn_tsn_manager;

/* fully configured 802.1Qcc CNC manager over the shared managers */
typedef struct {
    wtsn_db *db;
    wtsn_event_bus *bus;
    wtsn_mqtt_client *mqtt;
} wtsn_tsn_manager_config;

wtsn_tsn_manager *wtsn_tsn_manager_create(const wtsn_tsn_manager_config *cfg);
void wtsn_tsn_manager_destroy(wtsn_tsn_manager *m);

/* set/replace the MQTT broker channel used for deploys (may be NULL) */
void wtsn_tsn_manager_set_mqtt(wtsn_tsn_manager *m, wtsn_mqtt_client *mqtt);

wtsn_error wtsn_tsn_manager_add(wtsn_tsn_manager *m, const wtsn_stream *s);
wtsn_error wtsn_tsn_manager_remove(wtsn_tsn_manager *m, const char *stream_id);
wtsn_error wtsn_tsn_manager_load(wtsn_tsn_manager *m, const char *stream_id, wtsn_stream *out);
void wtsn_tsn_manager_for_each(wtsn_tsn_manager *m, int (*cb)(const wtsn_stream *s, void *ud), void *ud);
size_t wtsn_tsn_manager_count(wtsn_tsn_manager *m);

/* 802.1Qcc: compute the path and push talker + listeners to their agents via MQTT */
wtsn_error wtsn_tsn_manager_deploy(wtsn_tsn_manager *m, const char *stream_id);
wtsn_error wtsn_tsn_manager_deploy_all(wtsn_tsn_manager *m);

#endif
