#ifndef WTSN_GCL_H
#define WTSN_GCL_H

#include "common/common.h"

#include <stdint.h>

#define WTSN_GATE_OPEN 0x01
#define WTSN_GATE_MAX_QUEUES 8
#define WTSN_GCL_MAX_ENTRIES 128

typedef struct {
    unsigned char gate_state;
    int64_t duration_ns;
} wtsn_gcl_entry;

typedef struct {
    wtsn_gcl_entry entries[WTSN_GCL_MAX_ENTRIES];
    size_t entry_count;
    int64_t cycle_time_ns;
} wtsn_gcl;

wtsn_error wtsn_gcl_init(wtsn_gcl *gcl, int64_t cycle_time_ns);
wtsn_error wtsn_gcl_add_entry(wtsn_gcl *gcl, unsigned char gate_state, int64_t duration_ns);
void wtsn_gcl_reset(wtsn_gcl *gcl);
int64_t wtsn_gcl_total_duration_ns(const wtsn_gcl *gcl);
bool wtsn_gcl_is_valid(const wtsn_gcl *gcl);
void wtsn_gcl_render_ascii(const wtsn_gcl *gcl, char *out, size_t out_size);

#endif
