#ifndef WTSN_TAS_H
#define WTSN_TAS_H

#include "common/common.h"
#include "tas/gcl.h"

typedef struct {
    char id[WTSN_MAX_STR];
    char name[WTSN_MAX_STR];
    int64_t cycle_time_ns;
    char deploy_target[WTSN_MAX_STR];
    wtsn_gcl gcl;
} wtsn_tas_schedule_model;

wtsn_error wtsn_tas_validate(const wtsn_tas_schedule_model *s);
wtsn_error wtsn_tas_generate_helper(wtsn_tas_schedule_model *s);

#endif
