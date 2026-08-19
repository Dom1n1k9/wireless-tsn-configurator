#ifndef WTSN_PUBSUB_H
#define WTSN_PUBSUB_H

#include "common/common.h"

#include <stdint.h>
#include <stdbool.h>

#define WTSN_PUBSUB_DATASET_NAME_MAX 64
#define WTSN_PUBSUB_FIELD_NAME_MAX 64
#define WTSN_PUBSUB_TOPIC_MAX 128
#define WTSN_PUBSUB_MAX_FIELDS 32

typedef enum {
    WTSN_PUBSUB_KIND_OPCUA = 0,
    WTSN_PUBSUB_KIND_SIMULATED,   /* loopback pubsub */
    WTSN_PUBSUB_KIND_MQTT
} wtsn_pubsub_kind;

typedef enum {
    WTSN_FIELD_DOUBLE = 0,
    WTSN_FIELD_INT32,
    WTSN_FIELD_UINT16,
    WTSN_FIELD_BOOL
} wtsn_pubsub_field_type;

typedef struct {
    char name[WTSN_PUBSUB_FIELD_NAME_MAX];
    wtsn_pubsub_field_type type;
    union {
        double d;
        int32_t i;
        uint16_t u;
        bool b;
    } value;} wtsn_pubsub_field;

typedef struct {
    char name[WTSN_PUBSUB_DATASET_NAME_MAX];
    char topic[WTSN_PUBSUB_TOPIC_MAX];
    wtsn_pubsub_field fields[WTSN_PUBSUB_MAX_FIELDS];
    size_t field_count;
    int64_t cycle_time_ns;
} wtsn_pubsub_dataset;

typedef struct wtsn_pubsub wtsn_pubsub;

/* backend interface implemented by OPC UA PubSub / MQTT / simulated */
typedef struct {
    wtsn_pubsub_kind kind;
    const char * (*name)(struct wtsn_pubsub *ps);
    wtsn_error (*start)(struct wtsn_pubsub *ps);
    wtsn_error (*stop)(struct wtsn_pubsub *ps);
    wtsn_error (*publish)(struct wtsn_pubsub *ps, const wtsn_pubsub_dataset *ds);
    int (*process)(struct wtsn_pubsub *ps, int timeout_ms);
    void *state;
} wtsn_pubsub_backend;

struct wtsn_pubsub {
    wtsn_pubsub_backend backend;
};

void wtsn_pubsub_init(wtsn_pubsub *ps, const wtsn_pubsub_backend *backend, void *state);
const char *wtsn_pubsub_name(wtsn_pubsub *ps);
wtsn_error wtsn_pubsub_start(wtsn_pubsub *ps);
wtsn_error wtsn_pubsub_stop(wtsn_pubsub *ps);
wtsn_error wtsn_pubsub_publish(wtsn_pubsub *ps, const wtsn_pubsub_dataset *ds);
int wtsn_pubsub_process(wtsn_pubsub *ps, int timeout_ms);

void wtsn_pubsub_field_set_double(wtsn_pubsub_field *f, const char *name, double v);
void wtsn_pubsub_field_set_int32(wtsn_pubsub_field *f, const char *name, int32_t v);
void wtsn_pubsub_field_set_uint16(wtsn_pubsub_field *f, const char *name, uint16_t v);
void wtsn_pubsub_field_set_bool(wtsn_pubsub_field *f, const char *name, bool v);

#endif
