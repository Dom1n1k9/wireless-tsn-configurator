#ifndef WTSN_TRACE_H
#define WTSN_TRACE_H

#include "common/common.h"
#include "db/db.h"
#include "mvc/event_bus.h"

#define WTSN_TRACE_MAX 1024
#define WTSN_TRACE_LINE 256

typedef enum {
    WTSN_TRACE_COMM = 0,
    WTSN_TRACE_FRAME,
    WTSN_TRACE_CONFIG,
    WTSN_TRACE_MULTICAST
} wtsn_trace_type;

typedef struct {
    char timestamp[32];
    wtsn_trace_type type;
    char source[WTSN_MAX_STR];
    char line[WTSN_TRACE_LINE];
} wtsn_trace_entry;

typedef struct wtsn_trace wtsn_trace;

wtsn_trace *wtsn_trace_create(wtsn_event_bus *bus);
wtsn_trace *wtsn_trace_create_persistent(wtsn_event_bus *bus, wtsn_db *db, size_t keep);
void wtsn_trace_destroy(wtsn_trace *t);

wtsn_error wtsn_trace_add_comm(wtsn_trace *t, const char *source, const char *msg);
wtsn_error wtsn_trace_add_frame(wtsn_trace *t, const char *source, const unsigned char *bytes, size_t len);
wtsn_error wtsn_trace_add_config(wtsn_trace *t, const char *source, const char *what);
wtsn_error wtsn_trace_add_multicast(wtsn_trace *t, const char *source, const char *group, const char *msg);

wtsn_trace_entry *wtsn_trace_entry_at(wtsn_trace *t, int index);
int wtsn_trace_count(wtsn_trace *t);
void wtsn_trace_clear(wtsn_trace *t);

#endif
