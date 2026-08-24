#include "trace/trace.h"

#include "mvc/model.h"
#include "common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TRACE_MODEL "trace"

struct wtsn_trace {
    wtsn_event_bus *bus;
    wtsn_model model;
    wtsn_trace_entry entries[WTSN_TRACE_MAX];
    int count;
    int head;
};

static void stamp(wtsn_trace_entry *e) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) { e->timestamp[0] = '\0'; return; }
    strftime(e->timestamp, sizeof(e->timestamp), "%H:%M:%S", tm);
}

wtsn_trace *wtsn_trace_create(wtsn_event_bus *bus) {
    wtsn_trace *t = calloc(1, sizeof(wtsn_trace));
    if (!t) return NULL;
    t->bus = bus;
    wtsn_model_init(&t->model, TRACE_MODEL, bus);
    return t;
}

void wtsn_trace_destroy(wtsn_trace *t) {
    free(t);
}

static void push(wtsn_trace *t, wtsn_trace_type type, const char *source, const char *line) {
    wtsn_trace_entry *e = &t->entries[t->head];
    memset(e, 0, sizeof(*e));
    memset(e->timestamp, 0, sizeof(e->timestamp));
    stamp(e);
    e->type = type;
    wtsn_strlcpy(e->source, source ? source : "-", sizeof(e->source));
    wtsn_strlcpy(e->line, line ? line : "", sizeof(e->line));
    t->head = (t->head + 1) % WTSN_TRACE_MAX;
    if (t->count < WTSN_TRACE_MAX) t->count++;
    wtsn_model_notify_data(&t->model, "entry", e);
}

wtsn_error wtsn_trace_add_comm(wtsn_trace *t, const char *source, const char *msg) {
    if (!t || !msg) return WTSN_ERR_INVALID_ARG;
    push(t, WTSN_TRACE_COMM, source, msg);
    return WTSN_OK;
}

wtsn_error wtsn_trace_add_frame(wtsn_trace *t, const char *source, const unsigned char *bytes, size_t len) {
    if (!t || (!bytes && len > 0)) return WTSN_ERR_INVALID_ARG;
    char line[WTSN_TRACE_LINE];
    size_t off = 0;
    for (size_t i = 0; i < len && off + 4 < sizeof(line); i++) {
        off += (size_t)snprintf(line + off, sizeof(line) - off, "%02X ", bytes[i]);
    }
    if (off == 0) { line[0] = '\0'; }
    else { line[off] = '\0'; }
    push(t, WTSN_TRACE_FRAME, source, line);
    return WTSN_OK;
}

wtsn_error wtsn_trace_add_config(wtsn_trace *t, const char *source, const char *what) {
    if (!t || !what) return WTSN_ERR_INVALID_ARG;
    push(t, WTSN_TRACE_CONFIG, source, what);
    return WTSN_OK;
}

wtsn_error wtsn_trace_add_multicast(wtsn_trace *t, const char *source, const char *group, const char *msg) {
    if (!t || !msg) return WTSN_ERR_INVALID_ARG;
    char line[WTSN_TRACE_LINE];
    snprintf(line, sizeof(line), "FX mcast -> %s: %s", group ? group : "?", msg);
    push(t, WTSN_TRACE_MULTICAST, source, line);
    return WTSN_OK;
}

wtsn_trace_entry *wtsn_trace_entry_at(wtsn_trace *t, int index) {
    if (!t || index < 0 || index >= t->count) return NULL;
    int idx = (t->head - 1 - index + WTSN_TRACE_MAX) % WTSN_TRACE_MAX;
    return &t->entries[idx];
}

int wtsn_trace_count(wtsn_trace *t) {
    return t ? t->count : 0;
}

void wtsn_trace_clear(wtsn_trace *t) {
    if (!t) return;
    t->count = 0;
    t->head = 0;
}
