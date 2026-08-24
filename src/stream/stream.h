#ifndef WTSN_STREAM_H
#define WTSN_STREAM_H

#include "common/common.h"

/* TSN stream status following the 802.1Qcc reservation lifecycle:
 * CONFIGURED - requested by CNC, waiting for reservation
 * READY      - reservation established, all endpoints configured
 * FAILED     - reservation could not be satisfied (no path / bad params)
 * STANDBY    - reserved but not active (e.g. redundancy partner)
 */
typedef enum {
    WTSN_STREAM_CONFIGURED = 0,
    WTSN_STREAM_READY = 1,
    WTSN_STREAM_FAILED = 2,
    WTSN_STREAM_STANDBY = 3
} wtsn_stream_status;

typedef enum {
    WTSN_STREAM_ROLE_TALKER = 0,
    WTSN_STREAM_ROLE_LISTENER = 1,
    WTSN_STREAM_ROLE_COUNT
} wtsn_stream_role;

/* A wildcard "all devices" member (802.1Qcc all-listeners) */
#define WTSN_STREAM_ALL_LISTENERS "*"

typedef struct {
    char stream_id[WTSN_MAX_STR];       /* IEEE 802.1Qtalker stream ID */
    char name[WTSN_MAX_STR];
    char talker[WTSN_MAX_STR];          /* talker device id */
    int vlan_id;                        /* 0 = none */
    int64_t max_latency_ns;           /* PCP / max latency */
    int64_t max_interval_ns;          /* stream interval / TASA */
    int priority;                     /* 0-7 */
    int data_frame_prio;            /* data frame priority */
    wtsn_stream_status status;
    char comment[WTSN_MAX_STR];
    /* members: talker at index 0, followed by listeners */
    char listeners[WTSN_MAX_DEVICES][WTSN_MAX_STR];
    size_t listener_count;
    char listener_all;            /* true if ALL listener wildcard set */
} wtsn_stream;

wtsn_error wtsn_stream_validate(const wtsn_stream *s);
wtsn_stream_status wtsn_stream_status_parse(const char *s);
const char *wtsn_stream_status_str(wtsn_stream_status st);
const char *wtsn_stream_role_str(wtsn_stream_role r);

#endif
