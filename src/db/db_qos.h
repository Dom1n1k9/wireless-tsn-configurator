#ifndef WTSN_DB_QOS_H
#define WTSN_DB_QOS_H

#include "db/db.h"

typedef struct {
    char device_id[WTSN_MAX_STR];
    int priority;
    int traffic_class;
    int bandwidth_kbps;
    int latency_ms;
    int preemption;
} wtsn_qos_config;

wtsn_error wtsn_db_qos_save(wtsn_db *db, const wtsn_qos_config *cfg);
wtsn_error wtsn_db_qos_load(wtsn_db *db, const char *device_id, wtsn_qos_config *out);
wtsn_error wtsn_db_qos_delete(wtsn_db *db, const char *device_id);

#endif
