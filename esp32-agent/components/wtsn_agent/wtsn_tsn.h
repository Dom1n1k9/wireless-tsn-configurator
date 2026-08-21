#ifndef WTSN_TSN_H
#define WTSN_TSN_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    /* current applied state, kept for status reporting */
    int priority;
    int traffic_class;
    int preemption;
    int vlan_id;
    int timesync_mode;
    int64_t tas_cycle_ns;
} wtsn_tsn_state;

wtsn_tsn_state *wtsn_tsn_get_state(void);

#endif
