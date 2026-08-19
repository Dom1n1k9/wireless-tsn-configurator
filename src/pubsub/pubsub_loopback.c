#include "pubsub/pubsub_loopback.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    wtsn_pubsub_loopback_cb cb;
    void *ud;
} loopback_state;

static loopback_state g_global;

static const char *loopback_name(struct wtsn_pubsub *ps) {
    (void)ps;
    return "loopback";
}

static wtsn_error loopback_start(struct wtsn_pubsub *ps) {
    (void)ps;
    return WTSN_OK;
}

static wtsn_error loopback_stop(struct wtsn_pubsub *ps) {
    (void)ps;
    return WTSN_OK;
}

static wtsn_error loopback_publish(struct wtsn_pubsub *ps, const wtsn_pubsub_dataset *ds) {
    (void)ps;
    if (ds && g_global.cb) g_global.cb(ds, g_global.ud);
    return WTSN_OK;
}

static int loopback_process(struct wtsn_pubsub *ps, int timeout_ms) {
    (void)ps;
    (void)timeout_ms;
    return 0;
}

wtsn_error wtsn_pubsub_loopback_backend(wtsn_pubsub_backend *out, const wtsn_pubsub_loopback_opts *opts) {
    if (!out) return WTSN_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->kind = WTSN_PUBSUB_KIND_SIMULATED;
    out->name = loopback_name;
    out->start = loopback_start;
    out->stop = loopback_stop;
    out->publish = loopback_publish;
    out->process = loopback_process;
    if (opts) {
        g_global.cb = opts->on_publish;
        g_global.ud = opts->ud;
    }
    return WTSN_OK;
}

void wtsn_pubsub_loopback_tap_hook(void (*cb)(const wtsn_pubsub_dataset *, void *), void *ud) {
    g_global.cb = cb;
    g_global.ud = ud;
}
