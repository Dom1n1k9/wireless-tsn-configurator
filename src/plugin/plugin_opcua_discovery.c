#include "plugin/plugin_api.h"
#include "common/str_util.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char endpoint[WTSN_MAX_STR];
    char namespace_filter[WTSN_MAX_STR];
} opcua_plugin_data;

static wtsn_error opcua_discover(wtsn_plugin *self, wtsn_device *out, int max, int *count) {
    opcua_plugin_data *d = (opcua_plugin_data *)self->userdata;
    (void)d;
    (void)out;
    (void)max;
    if (count) *count = 0;
    return WTSN_OK;
}

static wtsn_error opcua_probe(wtsn_plugin *self, const char *discovery_data) {
    (void)self;
    if (!discovery_data) return WTSN_ERR_INVALID_ARG;
    return wtsn_str_starts_with(discovery_data, "opc.tcp:") ? WTSN_OK : WTSN_ERR_NOT_FOUND;
}

static wtsn_error opcua_shutdown(wtsn_plugin *self) {
    opcua_plugin_data *d = (opcua_plugin_data *)self->userdata;
    if (d) free(d);
    return WTSN_OK;
}

wtsn_plugin *wtsn_plugin_create(void) {
    wtsn_plugin *p = calloc(1, sizeof(wtsn_plugin));
    if (!p) return NULL;
    p->api_version = WTSN_PLUGIN_API_VERSION;
    wtsn_strlcpy(p->name, "opcua-discovery", sizeof(p->name));
    opcua_plugin_data *d = calloc(1, sizeof(opcua_plugin_data));
    wtsn_strlcpy(d->endpoint, "opc.tcp://localhost:4840", sizeof(d->endpoint));
    wtsn_strlcpy(d->namespace_filter, "urn:wtsn", sizeof(d->namespace_filter));
    p->userdata = d;
    p->probe = opcua_probe;
    p->discover = opcua_discover;
    p->shutdown = opcua_shutdown;
    return p;
}
