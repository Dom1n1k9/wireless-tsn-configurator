#include "tas/tas.h"

#include <string.h>

wtsn_error wtsn_tas_validate(const wtsn_tas_schedule_model *s) {
    if (!s) return WTSN_ERR_INVALID_ARG;
    if (s->cycle_time_ns <= 0) return WTSN_ERR_INVALID_ARG;
    if (!wtsn_gcl_is_valid(&s->gcl)) return WTSN_ERR_INVALID_ARG;
    if (strlen(s->name) == 0) return WTSN_ERR_INVALID_ARG;
    return WTSN_OK;
}

wtsn_error wtsn_tas_generate_helper(wtsn_tas_schedule_model *s) {
    if (!s || s->cycle_time_ns <= 0) return WTSN_ERR_INVALID_ARG;
    memset(&s->gcl, 0, sizeof(s->gcl));
    s->gcl.cycle_time_ns = s->cycle_time_ns;
    /* helper: single open window covering entire cycle */
    wtsn_gcl_add_entry(&s->gcl, WTSN_GATE_OPEN, s->cycle_time_ns);
    return WTSN_OK;
}
