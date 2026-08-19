#ifndef WTSN_PUBSUB_LOOPBACK_H
#define WTSN_PUBSUB_LOOPBACK_H

#include "pubsub/pubsub.h"

typedef void (*wtsn_pubsub_loopback_cb)(const wtsn_pubsub_dataset *ds, void *ud);

typedef struct {
    wtsn_pubsub_loopback_cb on_publish;
    void *ud;
} wtsn_pubsub_loopback_opts;

wtsn_error wtsn_pubsub_loopback_backend(wtsn_pubsub_backend *out, const wtsn_pubsub_loopback_opts *opts);
void wtsn_pubsub_loopback_tap_hook(void (*cb)(const wtsn_pubsub_dataset *, void *), void *ud);

#endif