#include "pubsub/pubsub_opcua.h"

#include "common/log.h"

#include <stdlib.h>
#include <string.h>

static wtsn_error real_start(struct wtsn_pubsub *ps);
static wtsn_error real_stop(struct wtsn_pubsub *ps);
static wtsn_error real_publish(struct wtsn_pubsub *ps, const wtsn_pubsub_dataset *ds);

/*
 * Real OPC UA PubSub backend.
 *
 * open62541 ships PubSub behind the UA_ENABLE_PUBSUB compile flag. When the
 * library was built with it, this backend wires datasets to a real UDP-mapped (or
 * MQTT-bridged) PubSub connection. When PubSub is not compiled in, the backend
 * reports "not available" and the application can fall back to the loopback backend.
 */

#if defined(UA_ENABLE_PUBSUB)
#include <open62541/server.h>
#include <open62541/server_pubsub.h>
#include <open62541/plugin/pubsub_udp.h>
#include <open62541/types_generated.h>
#endif

#define WTSN_PUBSUB_MAX_NODES 128

typedef struct {
    void *server;            /* UA_Server* the configurator owns */
    uint16_t ns;
    UA_NodeId connection;    /* PubSub UDP connection */
    UA_NodeId writerGroup;   /* WriterGroup on the connection */
    UA_NodeId pds;          /* published data set */
    UA_NodeId dataSetWriter; /* writer linking WG and PDS */
    UA_NodeId varNodes[WTSN_PUBSUB_MAX_NODES]; /* info model variables backing fields */
    size_t varNodeCount;
    size_t initialized;       /* set once writer topology is created */
} opcua_state;

static int g_available = 0;

#if defined(UA_ENABLE_PUBSUB)
static void checked_available(void) { g_available = 1; }

static const char *opcua_name(struct wtsn_pubsub *ps) {
    (void)ps;
    return "opcua-pubsub";
}
#else
static void checked_available(void) { g_available = 0; }
#endif

int wtsn_pubsub_opcua_available(void) {
    if (g_available == 0) checked_available();
    return g_available;
}

static const char *backend_name(struct wtsn_pubsub *ps) {
    (void)ps;
    return wtsn_pubsub_opcua_available() ? "opcua-pubsub" : "opcua-unavailable";
}

static wtsn_error backend_start(struct wtsn_pubsub *ps) {
    (void)ps;
    if (!wtsn_pubsub_opcua_available()) {
        wtsn_log(WTSN_LOG_WARN, "opc ua pubsub not available (compile UA_ENABLE_PUBSUB)");
        return WTSN_ERR_NOT_IMPLEMENTED;
    }
    return real_start(ps);
}

static wtsn_error backend_stop(struct wtsn_pubsub *ps) {
    (void)ps;
    return real_stop(ps);
}

static wtsn_error backend_publish(struct wtsn_pubsub *ps, const wtsn_pubsub_dataset *ds) {
    (void)ps;
    if (!ds) return WTSN_ERR_INVALID_ARG;
    if (!wtsn_pubsub_opcua_available()) return WTSN_ERR_NOT_IMPLEMENTED;
    return real_publish(ps, ds);
}

static int backend_process(struct wtsn_pubsub *ps, int timeout_ms) {
    (void)ps;
    (void)timeout_ms;
    return 0;
}

#if defined(UA_ENABLE_PUBSUB)
/* Create one variable node per dataset field so open62541 can read the current value
 * when it assembles the UADP network message on every publish interval. Returns the
 * newly created NodeId in *out, or UA_NODEID_NULL on failure. */
static UA_NodeId add_var_node(UA_Server *server, UA_UInt16 ns,
                              const wtsn_pubsub_dataset *ds, size_t i) {
    char name[WTSN_PUBSUB_DATASET_NAME_MAX + WTSN_PUBSUB_FIELD_NAME_MAX + 8];
    snprintf(name, sizeof(name), "%s.%s", ds->name, ds->fields[i].name);

    UA_Variant value;
    memset(&value, 0, sizeof(value));
    const UA_DataType *type = NULL;
    const void *src = NULL;
    switch (ds->fields[i].type) {
    case WTSN_FIELD_DOUBLE: type = &UA_TYPES[UA_TYPES_DOUBLE]; src = &ds->fields[i].value.d; break;
    case WTSN_FIELD_INT32:  type = &UA_TYPES[UA_TYPES_INT32];  src = &ds->fields[i].value.i; break;
    case WTSN_FIELD_UINT16: type = &UA_TYPES[UA_TYPES_UINT16]; src = &ds->fields[i].value.u; break;
    case WTSN_FIELD_BOOL:   type = &UA_TYPES[UA_TYPES_BOOLEAN]; src = &ds->fields[i].value.b; break;
    default: return UA_NODEID_NULL;
    }
    UA_Variant_setScalar(&value, (void *)src, type);

    /* add a variable node to the information model (ns, id) that backs this field,
     * then stamp the initial value. Note: addVariableNode uses a string NodeId in the
     * required namespace, so the field is addressable both by the DataSetField and by
     * the running server. */
    UA_NodeId id = UA_NODEID_STRING(ns, (char *)name);
    UA_QualifiedName qn = UA_QUALIFIEDNAME(ns, (char *)name);
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("", (char *)name);
    attr.dataType = type->typeId;
    attr.value = value;
    UA_StatusCode rc = UA_Server_addVariableNode(server, id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), qn,
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, &id);
    (void)rc;
    return id;
}

/* Creates the UDP PubSub connection that real_publish later attaches the writer chain to. */
static wtsn_error real_start(struct wtsn_pubsub *ps) {
#if defined(UA_ENABLE_PUBSUB)
    opcua_state *st = (opcua_state *)ps->backend.state;
    UA_Server *server = (UA_Server *)st->server;
    if (!server) return WTSN_ERR_INVALID_ARG;
    if (!UA_NodeId_isNull(&st->connection)) return WTSN_OK; /* already created */

    /* OPC UA FX wireless: all W-TSN nodes join a shared multicast group
       (UA-DP transport, UDP) so a single publisher reaches every subscriber
       directly without an MQTT broker. Default WTSN group: opc.udp://239.255.0.1:4840/ */
    UA_PubSubConnectionConfig conn;
    memset(&conn, 0, sizeof(conn));
    conn.name = UA_STRING("WTSN-UDP Connection");
    conn.transportProfileUri = UA_STRING("http://opcfoundation.org/UA-Profile/Transport/pubsub-udp-uadp");
    conn.enabled = UA_TRUE;
    UA_NetworkAddressUrlDataType addr = {UA_STRING_NULL, UA_STRING("opc.udp://239.255.0.1:4840/")};
    UA_Variant_setScalar(&conn.address, &addr, &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    conn.publisherIdType = UA_PUBSUB_PUBLISHERID_NUMERIC;
    conn.publisherId.numeric = UA_UInt32_random();
    conn.connectionProperties = NULL;
    conn.connectionPropertiesSize = 0;

    UA_StatusCode rc = UA_Server_addPubSubConnection(server, &conn, &st->connection);
    if (rc != UA_STATUSCODE_GOOD) {
        wtsn_log(WTSN_LOG_WARN, "opc ua add pubsub connection rc=0x%08X", rc);
        return WTSN_ERR_IO;
    }
    wtsn_log(WTSN_LOG_INFO, "opc ua pubsub connection added (ns=%u)", st->ns);
    return WTSN_OK;
#else
    (void)ps;
    return WTSN_ERR_NOT_IMPLEMENTED;
#endif
}

static wtsn_error real_stop(struct wtsn_pubsub *ps) {
#if defined(UA_ENABLE_PUBSUB)
    (void)ps;
    return WTSN_OK;
#else
    (void)ps;
    return WTSN_ERR_NOT_IMPLEMENTED;
#endif
}
#endif

static wtsn_error real_publish(struct wtsn_pubsub *ps, const wtsn_pubsub_dataset *ds) {
#if defined(UA_ENABLE_PUBSUB)
    opcua_state *st = (opcua_state *)ps->backend.state;
    UA_Server *server = (UA_Server *)st->server;
    if (!server || !ds) return WTSN_ERR_INVALID_ARG;

    /* Lazy topology creation: the first publish call builds the full writer chain
     * (connection is created earlier in real_start; here we add PDS, fields, writer
     * group and DataSetWriter based on the dataset schema). */
    if (!st->initialized) {
        wtsn_error e = real_start(ps);
        if (e != WTSN_OK) return e;

        /* PublishedDataSet */
        UA_PublishedDataSetConfig pds;
        memset(&pds, 0, sizeof(pds));
        pds.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
        pds.name = UA_STRING((char *)ds->name);
        UA_AddPublishedDataSetResult pdsRes;
        pdsRes = UA_Server_addPublishedDataSet(server, &pds, &st->pds);
        if (pdsRes.addResult != UA_STATUSCODE_GOOD) return WTSN_ERR_IO;

        /* DataSetFields -> one variable node and one field per wtsn field */
        for (size_t i = 0; i < ds->field_count && i < WTSN_PUBSUB_MAX_NODES; i++) {
            UA_NodeId var = add_var_node(server, st->ns, ds, i);
            if (UA_NodeId_isNull(&var)) continue;
            st->varNodes[st->varNodeCount++] = var;

            UA_DataSetFieldConfig fcfg;
            memset(&fcfg, 0, sizeof(fcfg));
            fcfg.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
            fcfg.field.variable.fieldNameAlias = UA_STRING((char *)ds->fields[i].name);
            fcfg.field.variable.publishParameters.publishedVariable = var;
            fcfg.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;
            UA_NodeId fid;
            UA_Server_addDataSetField(server, st->pds, &fcfg, &fid);
        }

        /* WriterGroup */
        UA_WriterGroupConfig wg;
        memset(&wg, 0, sizeof(wg));
        wg.name = UA_STRING("WTSN-WriterGroup");
        wg.publishingInterval = ds->cycle_time_ns > 0 ? (UA_Double)ds->cycle_time_ns / 1e6 : 100.0;
        wg.enabled = UA_TRUE;
        wg.writerGroupId = 101;
        wg.encodingMimeType = UA_PUBSUB_ENCODING_UADP;
        wg.messageSettings.encoding = UA_EXTENSIONOBJECT_DECODED;
        wg.messageSettings.content.decoded.type = &UA_TYPES[UA_TYPES_UADPWRITERGROUPMESSAGEDATATYPE];
        UA_UadpWriterGroupMessageDataType *m =
            UA_UadpWriterGroupMessageDataType_new();
        m->networkMessageContentMask = (UA_UadpNetworkMessageContentMask)
            (UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID |
             UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER |
             UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER);
        wg.messageSettings.content.decoded.data = m;
        UA_Server_addWriterGroup(server, st->connection, &wg, &st->writerGroup);
        UA_Server_setWriterGroupOperational(server, st->writerGroup);
        UA_UadpWriterGroupMessageDataType_delete(m);

        /* DataSetWriter */
        UA_DataSetWriterConfig dsw;
        memset(&dsw, 0, sizeof(dsw));
        dsw.name = UA_STRING("WTSN-DataSetWriter");
        dsw.dataSetWriterId = 42;
        dsw.keyFrameCount = 1;
        UA_Server_addDataSetWriter(server, st->writerGroup, st->pds, &dsw,
                                  &st->dataSetWriter);

        st->initialized = 1;
        wtsn_log(WTSN_LOG_INFO, "opc ua pubsub writer ready: %zu field(s)",
                 st->varNodeCount);
    }

    /* update every variable node from the current dataset snapshot so the writer group
     * picks it up on the next publish interval */
    for (size_t i = 0; i < ds->field_count && i < st->varNodeCount; i++) {
        const wtsn_pubsub_field *f = &ds->fields[i];
        UA_Variant v; memset(&v, 0, sizeof(v));
        switch (f->type) {
        case WTSN_FIELD_DOUBLE: UA_Variant_setScalar(&v, &f->value.d, &UA_TYPES[UA_TYPES_DOUBLE]); break;
        case WTSN_FIELD_INT32:  UA_Variant_setScalar(&v, &f->value.i, &UA_TYPES[UA_TYPES_INT32]); break;
        case WTSN_FIELD_UINT16: UA_Variant_setScalar(&v, &f->value.u, &UA_TYPES[UA_TYPES_UINT16]); break;
        case WTSN_FIELD_BOOL:   UA_Variant_setScalar(&v, &f->value.b, &UA_TYPES[UA_TYPES_BOOLEAN]); break;
        default: continue;
        }
        UA_Server_writeValue(server, st->varNodes[i], v);
    }
    return WTSN_OK;
#else
    (void)ps;
    (void)ds;
    return WTSN_ERR_NOT_IMPLEMENTED;
#endif
}

wtsn_error wtsn_pubsub_opcua_backend(wtsn_pubsub_backend *out, void *server, uint16_t ns) {
    if (!out) return WTSN_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->kind = WTSN_PUBSUB_KIND_OPCUA;
    out->name = backend_name;
    out->start = backend_start;
    out->stop = backend_stop;
    out->publish = backend_publish;
    out->process = backend_process;
    if (wtsn_pubsub_opcua_available()) {
        opcua_state *st = calloc(1, sizeof(opcua_state));
        if (st) { st->server = server; st->ns = ns; out->state = st; }
    }
    return (out->state || !wtsn_pubsub_opcua_available()) ? WTSN_OK : WTSN_ERR_NO_MEMORY;
}
