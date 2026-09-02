#include "discovery/discovery.h"

#include "common/str_util.h"

#include <stdlib.h>
#include <string.h>

wtsn_discoverer *wtsn_discovery_create(wtsn_discovery_source src, const char *name,
                                       wtsn_discovery_run_fn run,
                                       wtsn_discovery_destroy_fn destroy, void *data) {
    wtsn_discoverer *d = calloc(1, sizeof(wtsn_discoverer));
    if (!d) return NULL;
    d->source = src;
    wtsn_strlcpy(d->name, name ? name : "discoverer", sizeof(d->name));
    d->run = run;
    d->destroy = destroy;
    d->data = data;
    return d;
}

void wtsn_discovery_destroy(wtsn_discoverer *d) {
    if (!d) return;
    if (d->destroy) d->destroy(d);
    free(d);
}
