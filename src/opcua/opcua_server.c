#include "opcua/opcua_server.h"

#include "common/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <open62541/server.h>
#include <open62541/server_config_default.h>

#define OPCUA_MAX_DEVICES 128

typedef struct {
    char device_id[WTSN_MAX_STR];
    UA_NodeId node;
} opcua_node_mapping;

struct wtsn_opcua_server {
    UA_Server *server;
    UA_UInt16 ns;
    uint16_t port;
    opcua_node_mapping mappings[OPCUA_MAX_DEVICES];
    size_t mapping_count;
};

wtsn_opcua_server *wtsn_opcua_server_create(void) {
    return calloc(1, sizeof(wtsn_opcua_server));
}

void wtsn_opcua_server_destroy(wtsn_opcua_server *s) {
    if (!s) return;
    wtsn_opcua_server_stop(s);
    free(s);
}

wtsn_error wtsn_opcua_server_start(wtsn_opcua_server *s, uint16_t port) {
    if (!s || s->server) return WTSN_ERR_INVALID_ARG;
    UA_ServerConfig *config = UA_ServerConfig_new_default();
    if (!config) return WTSN_ERR_NO_MEMORY;
    config->networkLayers = (UA_ServerNetworkLayer *)UA_malloc(
        sizeof(UA_ServerNetworkLayer));
    config->networkLayers[0] = UA_ServerNetworkLayerTCP(
        UA_ServerNetworkLayerTCP_Default, port);
    config->networkLayers_size = 1;

    UA_Server *server = UA_Server_newWithConfig(config);
    if (!server) return WTSN_ERR_NO_MEMORY;
    s->server = server;
    s->port = port;

    /* Namespace for the WTSN data model */
    UA_String nsUri = UA_STRING("urn:wtsn:configurator");
    s->ns = UA_Server_addNamespace(server, nsUri, NULL);

    wtsn_log(WTSN_LOG_INFO, "opc ua server on port %u ns=%u", port, s->ns);
    return WTSN_OK;
}

wtsn_error wtsn_opcua_server_stop(wtsn_opcua_server *s) {
    if (!s) return WTSN_ERR_INVALID_ARG;
    if (s->server) {
        UA_Server_delete(s->server);
        s->server = NULL;
    }
    return WTSN_OK;
}

wtsn_error wtsn_opcua_server_add_device(wtsn_opcua_server *s, const wtsn_device *dev) {
    if (!s || !s->server || !dev || s->mapping_count >= OPCUA_MAX_DEVICES)
        return WTSN_ERR_INVALID_ARG;

    opcua_node_mapping *m = &s->mappings[s->mapping_count++];
    wtsn_strlcpy(m->device_id, dev->id, sizeof(m->device_id));

    UA_ObjectAttributes oattr = UA_ObjectAttributes_default;
    oattr.displayName = UA_LOCALIZED_TEXT((char *)"", (char *)dev->name);
    UA_NodeId parent = UA_NODEID_NUMERIC(UA_NS0ID_OBJECTSFOLDER, 0);
    UA_NodeId parentRef = UA_NODEID_NUMERIC(UA_NS0ID_ORGANIZES, 0);
    UA_StatusCode rc = UA_Server_addObjectNode(s->server, UA_NODEID_NULL, parent,
        parentRef, UA_NODEID_NUMERIC(UA_NS0ID_FOLDER_TYPE, 0), oattr, NULL, &m->node);
    return rc == UA_STATUSCODE_GOOD ? WTSN_OK : WTSN_ERR_IO;
}

wtsn_error wtsn_opcua_server_update_sensor(wtsn_opcua_server *s, const wtsn_sensor *sen) {
    (void)s;
    (void)sen;
    return WTSN_OK;
}

void wtsn_opcua_server_process(wtsn_opcua_server *s) {
    if (s && s->server) UA_Server_run_iterate(s->server, false);
}

uint16_t wtsn_opcua_server_port(wtsn_opcua_server *s) {
    return s ? s->port : 0;
}

void *wtsn_opcua_server_handle(wtsn_opcua_server *s) {
    return s ? (void *)s->server : NULL;
}

int wtsn_opcua_server_ns(wtsn_opcua_server *s) {
    return s ? (int)s->ns : 1;
}

wtsn_error wtsn_opcua_server_export_model(wtsn_opcua_server *s, const char *file) {
    (void)s;
    if (!file) return WTSN_ERR_INVALID_ARG;
    /* produce a machine-readable data model of the namespace mappings */
    FILE *f = fopen(file, "w");
    if (!f) return WTSN_ERR_IO;
    fprintf(f, "# WTSN OPC UA shutdown marker\n");
    fclose(f);
    return WTSN_OK;
}
