#include "stream/tsn_manager.h"

#include "mqtt/mqtt_client.h"

#include "stream/stream.h"

#include "common/log.h"
#include "common/str_util.h"
#include "db/db_devices.h"
#include "db/db_tsn.h"
#include "mvc/model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TSN_MODEL "tsn"

struct wtsn_tsn_manager {
    wtsn_db *db;
    wtsn_event_bus *bus;
    wtsn_mqtt_client *mqtt;
    wtsn_model model;
    wtsn_stream streams[WTSN_MAX_DEVICES];
    size_t count;
};

static int collect_stream(const wtsn_stream *s, void *ud) {
    wtsn_tsn_manager *m = (wtsn_tsn_manager *)ud;
    if (m->count < WTSN_MAX_DEVICES) m->streams[m->count++] = *s;
    return 0;
}

static void notify(wtsn_tsn_manager *m, const char *ev, const wtsn_stream *s) {
    char buf[WTSN_MAX_STR];
    snprintf(buf, sizeof(buf), "%s", ev);
    wtsn_model_notify_data(&m->model, buf, (void *)s);
}

wtsn_tsn_manager *wtsn_tsn_manager_create(const wtsn_tsn_manager_config *cfg) {
    if (!cfg || !cfg->db) return NULL;
    wtsn_tsn_manager *m = calloc(1, sizeof(wtsn_tsn_manager));
    if (!m) return NULL;
    m->db = cfg->db;
    m->bus = cfg->bus;
    m->mqtt = cfg->mqtt;
    wtsn_model_init(&m->model, TSN_MODEL, m->bus);
    wtsn_db_tsn_for_each(m->db, collect_stream, m);
    return m;
}

void wtsn_tsn_manager_destroy(wtsn_tsn_manager *m) {
    free(m);
}

void wtsn_tsn_manager_set_mqtt(wtsn_tsn_manager *m, wtsn_mqtt_client *mqtt) {
    if (m) m->mqtt = mqtt;
}

size_t wtsn_tsn_manager_count(wtsn_tsn_manager *m) {
    return m ? m->count : 0;
}

wtsn_error wtsn_tsn_manager_add(wtsn_tsn_manager *m, const wtsn_stream *s) {
    if (!m || !s) return WTSN_ERR_INVALID_ARG;
    wtsn_error e = wtsn_stream_validate(s);
    if (e != WTSN_OK) return e;

    bool exists = false;
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->streams[i].stream_id, s->stream_id) == 0) {
            m->streams[i] = *s;
            exists = true;
            break;
        }
    }
    if (!exists) {
        if (m->count >= WTSN_MAX_DEVICES) return WTSN_ERR_BUSY;
        m->streams[m->count++] = *s;
    }
    wtsn_db_tsn_save(m->db, s);
    notify(m, exists ? "updated" : "created", s);
    return WTSN_OK;
}

wtsn_error wtsn_tsn_manager_remove(wtsn_tsn_manager *m, const char *stream_id) {
    if (!m || !stream_id) return WTSN_ERR_INVALID_ARG;
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->streams[i].stream_id, stream_id) == 0) {
            wtsn_stream removed = m->streams[i];
            memmove(&m->streams[i], &m->streams[i+1], (m->count - i - 1) * sizeof(wtsn_stream));
            m->count--;
            wtsn_db_tsn_delete(m->db, stream_id);
            notify(m, "removed", &removed);
            return WTSN_OK;
        }
    }
    return WTSN_ERR_NOT_FOUND;
}

wtsn_error wtsn_tsn_manager_load(wtsn_tsn_manager *m, const char *stream_id, wtsn_stream *out) {
    if (!m || !stream_id || !out) return WTSN_ERR_INVALID_ARG;
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->streams[i].stream_id, stream_id) == 0) { *out = m->streams[i]; return WTSN_OK; }
    }
    return WTSN_ERR_NOT_FOUND;
}

void wtsn_tsn_manager_for_each(wtsn_tsn_manager *m, int (*cb)(const wtsn_stream *s, void *ud), void *ud) {
    if (!m || !cb) return;
    for (size_t i = 0; i < m->count; i++) cb(&m->streams[i], ud);
}

/* ---- 802.1Qcc deploy: push stream to talker + listeners via MQTT ----
   topic: tsn/cmd/<device>/stream  payload: JSON snapshot of the stream.
   Uses the established MQTT client. */
static wtsn_error publish_stream(wtsn_tsn_manager *m, const char *device,
                               const wtsn_stream *s, wtsn_stream_role role) {
    if (!m || !m->mqtt || !device) return WTSN_ERR_NOT_READY;
    char payload[1024];
    size_t off = (size_t)snprintf(payload, sizeof(payload),
        "{\"stream\":\"%s\",\"name\":\"%s\",\"role\":\"%s\",\"talker\":\"%s\","
        "\"vlan_id\":%d,\"latency_ns\":%lld,\"interval_ns\":%lld,"
        "\"priority\":%d,\"dfprio\":%d,\"status\":\"%s\"}",
        s->stream_id, s->name, wtsn_stream_role_str(role), s->talker,
        s->vlan_id, (long long)s->max_latency_ns, (long long)s->max_interval_ns,
        s->priority, s->data_frame_prio, wtsn_stream_status_str(s->status));
    char topic[WTSN_MAX_STR];
    snprintf(topic, sizeof(topic), "tsn/cmd/%s/stream", device);
    if (wtsn_mqtt_client_publish(m->mqtt, topic, payload) == WTSN_OK) {
        wtsn_log(WTSN_LOG_INFO, "stream %s -> %s (%s): %s",
                 s->stream_id, device, wtsn_stream_role_str(role), payload);
        return WTSN_OK;
    }
    return WTSN_ERR_NET;
}

struct listener_walk_ctx {
    wtsn_tsn_manager *m;
    const wtsn_stream *s;
    const char *domain;     /* NULL = all domains */
};

static void all_listener_cb(const wtsn_device *dev, void *userdata) {
    struct listener_walk_ctx *ctx = (struct listener_walk_ctx *)userdata;
    if (!dev->id[0] || strcmp(dev->id, ctx->s->talker) == 0) return;
    if (ctx->domain && strcmp(dev->domain, ctx->domain) != 0) return;
    publish_stream(ctx->m, dev->id, ctx->s, WTSN_STREAM_ROLE_LISTENER);
}

static wtsn_error deploy_scoped(wtsn_tsn_manager *m, const char *stream_id,
                                const char *domain);

wtsn_error wtsn_tsn_manager_deploy(wtsn_tsn_manager *m, const char *stream_id) {
    return deploy_scoped(m, stream_id, NULL);
}

/* Internal scoped deploy: `domain` (NULL/empty = all domains) also limits
 * the all-listeners fan-out so a domain-scoped deploy never configures
 * endpoints outside its own physical cell. */
static wtsn_error deploy_scoped(wtsn_tsn_manager *m, const char *stream_id,
                                const char *domain) {
    if (!m || !stream_id) return WTSN_ERR_INVALID_ARG;
    wtsn_stream s;
    wtsn_error e = wtsn_tsn_manager_load(m, stream_id, &s);
    if (e != WTSN_OK) return e;
    if (!m->mqtt) return WTSN_ERR_NOT_READY;

    if (publish_stream(m, s.talker, &s, WTSN_STREAM_ROLE_TALKER) != WTSN_OK)
        return WTSN_ERR_NET;

    if (s.listener_all) {
        /* all-listeners: push the stream to every device in the DB except
           the talker, so each agent registers itself as a listener. When a
           domain scope is active, only devices of that domain are reached. */
        struct listener_walk_ctx ctx = { m, &s, domain };
        if (domain && domain[0])
            (void)wtsn_db_device_for_each_in_domain(m->db, domain, all_listener_cb, &ctx);
        else
            (void)wtsn_db_device_for_each(m->db, all_listener_cb, &ctx);
    } else {
        for (size_t i = 0; i < s.listener_count; i++) {
            if (!s.listeners[i][0]) continue;
            publish_stream(m, s.listeners[i], &s, WTSN_STREAM_ROLE_LISTENER);
        }
    }
    wtsn_db_tsn_set_status(m->db, stream_id, WTSN_STREAM_READY);
    s.status = WTSN_STREAM_READY;
    notify(m, "deployed", &s);
    return WTSN_OK;
}

static int collect_order(const wtsn_stream *s, void *ud) {
    (void)s;
    (void)ud;
    return 0;
}

/* True when the stream belongs to `domain` (via its talker device), or when
 * the scope argument is empty (global deploy). A device missing from the DB
 * is treated as "belongs" so a domain scope never silently loses a stream
 * that still needs its endpoints pushed. */
static int stream_in_domain(wtsn_tsn_manager *m, const char *stream_id,
                            const char *domain) {
    if (!domain || !domain[0]) return 1;
    wtsn_stream s;
    if (wtsn_tsn_manager_load(m, stream_id, &s) != WTSN_OK) return 1;
    wtsn_device talker;
    if (wtsn_db_device_get(m->db, s.talker, &talker) != WTSN_OK) return 1;
    return !talker.domain[0] || strcmp(talker.domain, domain) == 0;
}

struct deploy_ctx {
    wtsn_tsn_manager *m;
    const char *domain;     /* NULL/empty = all domains */
};

static int deploy_cb(const wtsn_stream *s, void *ud) {
    struct deploy_ctx *dctx = (struct deploy_ctx *)ud;
    if (!stream_in_domain(dctx->m, s->stream_id, dctx->domain)) return 0;
    return deploy_scoped(dctx->m, s->stream_id, dctx->domain) == WTSN_OK ? 0 : 0;
}

wtsn_error wtsn_tsn_manager_deploy_all(wtsn_tsn_manager *m, const char *domain) {
    if (!m) return WTSN_ERR_INVALID_ARG;
    (void)collect_order;
    struct deploy_ctx dctx = { m, domain };
    wtsn_tsn_manager_for_each(m, deploy_cb, &dctx);
    return WTSN_OK;
}
