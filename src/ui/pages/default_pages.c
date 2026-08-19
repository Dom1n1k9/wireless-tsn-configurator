#include "ui/pages/pages.h"
#include "ui/widgets/theme.h"
#include "device/device.h"
#include "db/db_vlan.h"
#include "db/db_tas.h"
#include "db/db_timesync.h"
#include "db/db_qos.h"
#include "db/db_sensors.h"

#include <lvgl/lvgl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opcua/opcua_server.h"
#include "pubsub/pubsub_opcua.h"

typedef struct { wtsn_app *app; } page_ctx;

/* ---- helpers ---- */

static lv_obj_t *row_card(lv_obj_t *parent, const char *title) {
    lv_obj_t *card = wtsn_ui_create_card(parent, title);
    lv_obj_set_size(card, lv_pct(96), LV_SIZE_CONTENT);
    // switch to column flow so following rows stack
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

static void kv(lv_obj_t *parent, const char *key, const char *fmt, ...) {
    char val[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(val, sizeof(val), fmt, ap);
    va_end(ap);
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, g_theme.text_dim, 0);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val);
    lv_obj_set_style_text_color(v, g_theme.text, 0);
}

static const char *dev_kind(wtsn_device_kind k) {
    switch (k) {
    case WTSN_DEVICE_KIND_ESP32: return "ESP32";
    case WTSN_DEVICE_KIND_RASPBERRYPI: return "Raspberry Pi";
    case WTSN_DEVICE_KIND_STM32: return "STM32";
    default: return "Generic";
    }
}

static const char *dev_status(wtsn_device_status s) {
    switch (s) {
    case WTSN_DEVICE_ONLINE: return "online";
    case WTSN_DEVICE_ERROR: return "error";
    default: return "offline";
    }
}

/* ---- devices page ---- */
typedef struct { lv_obj_t *parent; int first; } devb;
static void dev_disp_cb(const wtsn_device *d, void *ud) {
    devb *b = (devb *)ud;
    if (b->first) { lv_obj_clean(b->parent); b->first = 0; }
    lv_obj_t *card = row_card(b->parent, d->id);
    kv(card, "name", "%s", d->name);
    kv(card, "status", "%s", dev_status(d->status));
    kv(card, "kind", "%s", dev_kind(d->kind));
    kv(card, "ip", "%s", d->ip);
    kv(card, "firmware", "%s", d->firmware);
    kv(card, "tsn features", "%zu", d->tsn_features_count);
    for (size_t i = 0; i < d->tsn_features_count; i++)
        kv(card, "  ", "%s", d->tsn_features[i]);
}
static void devices_activate(wtsn_view *self) {
    page_ctx *ctx = (page_ctx *)self->userdata;
    lv_obj_clean(g_ui_content);
    lv_obj_t *scroll = lv_obj_create(g_ui_content);
    lv_obj_set_size(scroll, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_layout(scroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    devb b = {scroll, 1};
    if (ctx->app->devices) wtsn_device_manager_for_each(ctx->app->devices, dev_disp_cb, &b);
    if (b.first) { lv_obj_t *c = row_card(scroll, "No devices yet"); kv(c, "", "Run the node simulator or agent to discover devices."); }
}

/* ---- QoS / tsn page ---- */
typedef struct { wtsn_app *app; lv_obj_t *parent; } qsc;
static void qos_cb(const wtsn_device *d, void *ud) {
    qsc *q = (qsc *)ud;
    wtsn_qos_config c;
    if (wtsn_db_qos_load(&q->app->db, d->id, &c) != WTSN_OK) return;
    lv_obj_t *card = row_card(q->parent, d->id);
    kv(card, "priority", "%d", c.priority);
    kv(card, "traffic class", "%d", c.traffic_class);
    kv(card, "bandwidth", "%d kbps", c.bandwidth_kbps);
    kv(card, "latency", "%d ms", c.latency_ms);
    kv(card, "preemption", "%s", wtsn_preemption_str((wtsn_frame_preemption)c.preemption));
}
static void tsn_activate(wtsn_view *self) {
    page_ctx *ctx = (page_ctx *)self->userdata;
    lv_obj_clean(g_ui_content);
    lv_obj_t *scroll = lv_obj_create(g_ui_content);
    lv_obj_set_size(scroll, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(scroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    qsc q = {ctx->app, scroll};
    if (ctx->app->devices) wtsn_device_manager_for_each(ctx->app->devices, qos_cb, &q);
}

/* ---- vlan page ---- */
typedef struct { lv_obj_t *parent; } vlc;
static int vlan_disp_cb(const wtsn_vlan_group *g, void *ud) {
    vlc *v = (vlc *)ud;
    lv_obj_t *card = row_card(v->parent, g->id);
    kv(card, "name", "%s", g->name);
    kv(card, "vlan id", "%d", g->vlan_id);
    return 0;
}
static void vlan_activate(wtsn_view *self) {
    page_ctx *ctx = (page_ctx *)self->userdata;
    lv_obj_clean(g_ui_content);
    lv_obj_t *scroll = lv_obj_create(g_ui_content);
    lv_obj_set_size(scroll, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(scroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    vlc v = {scroll};
    wtsn_db_vlan_group_for_each(&ctx->app->db, vlan_disp_cb, &v);
}

/* ---- timesync page ---- */
static void timesync_activate(wtsn_view *self) {
    page_ctx *ctx = (page_ctx *)self->userdata;
    lv_obj_clean(g_ui_content);
    lv_obj_t *scroll = lv_obj_create(g_ui_content);
    lv_obj_set_size(scroll, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(scroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);

    wtsn_timesync_status st;
    if (wtsn_db_timesync_load(&ctx->app->db, &st) == WTSN_OK) {
        lv_obj_t *card = row_card(scroll, "gPTP / 802.1AS");
        kv(card, "mode", "%s", wtsn_timesync_mode_str(st.mode));
        kv(card, "grandmaster", "%s", st.grandmaster[0] ? st.grandmaster : "auto");
        kv(card, "offset", "%lld ns", (long long)st.offset_ns);
        kv(card, "quality", "%d", st.quality);
    } else {
        lv_obj_t *card = row_card(scroll, "Time Sync");
        kv(card, "", "No time-sync status configured yet.");
    }
}

/* ---- tas page ---- */
typedef struct { lv_obj_t *parent; } tasc;
static int tas_disp_cb(const wtsn_tas_schedule *s, void *ud) {
    tasc *t = (tasc *)ud;
    lv_obj_t *card = row_card(t->parent, s->name);
    kv(card, "id", "%s", s->id);
    kv(card, "cycle", "%lld ns", (long long)s->cycle_time_ns);
    kv(card, "deploy target", "%s", s->deploy_target);
    kv(card, "GCL entries", "%zu", s->entry_count);
    return 0;
}
static void tas_activate(wtsn_view *self) {
    page_ctx *ctx = (page_ctx *)self->userdata;
    lv_obj_clean(g_ui_content);
    lv_obj_t *scroll = lv_obj_create(g_ui_content);
    lv_obj_set_size(scroll, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(scroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    tasc t = {scroll};
    wtsn_db_tas_for_each(&ctx->app->db, tas_disp_cb, &t);
}

/* ---- sensors page ---- */
typedef struct { lv_obj_t *parent; } senc;
static void sensor_disp_cb(const wtsn_sensor *s, void *ud) {
    senc *c = (senc *)ud;
    lv_obj_t *card = row_card(c->parent, s->device_id);
    kv(card, "sensor", "%s", s->sensor_id);
    kv(card, "type", "%s", wtsn_sensor_type_str(s->type));
    kv(card, "value", "%.2f %s", s->value, s->unit);
    kv(card, "healthy", "%s", s->healthy ? "yes" : "no");
}
static void sensors_activate(wtsn_view *self) {
    page_ctx *ctx = (page_ctx *)self->userdata;
    lv_obj_clean(g_ui_content);
    lv_obj_t *scroll = lv_obj_create(g_ui_content);
    lv_obj_set_size(scroll, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(scroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    senc c = {scroll};
    if (ctx->app->sensors) wtsn_sensor_manager_for_each(ctx->app->sensors, sensor_disp_cb, &c);
}

/* ---- opcua page ---- */
static void opcua_activate(wtsn_view *self) {
    page_ctx *ctx = (page_ctx *)self->userdata;
    lv_obj_clean(g_ui_content);
    lv_obj_t *scroll = lv_obj_create(g_ui_content);
    lv_obj_set_size(scroll, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(scroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *card = row_card(scroll, "OPC UA Server");
    kv(card, "port", "%u", (unsigned)wtsn_opcua_server_port(ctx->app->opcua));
    kv(card, "namespace", "%d", wtsn_opcua_server_ns(ctx->app->opcua));
    kv(card, "pubsub", "%s", ctx->app->pubsub ? wtsn_pubsub_name(ctx->app->pubsub) : "none");
    lv_obj_t *c2 = row_card(scroll, "FX / Wireless Multicast");
    kv(c2, "", "All W-TSN members join 239.255.0.1:4840 (UA-DP), no broker needed.");
    kv(c2, "", wtsn_pubsub_opcua_available() ? "Real PubSub backend active (UADP/UDP)."
                                             : "Simulated loopback backend (no UA_ENABLE_PUBSUB).");
}

/* ---- mqtt page ---- */
static void mqtt_activate(wtsn_view *self) {
    page_ctx *ctx = (page_ctx *)self->userdata;
    lv_obj_clean(g_ui_content);
    lv_obj_t *scroll = lv_obj_create(g_ui_content);
    lv_obj_set_size(scroll, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(scroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *card = row_card(scroll, "MQTT Client");
    kv(card, "broker", "%s:%d", ctx->app->config.mqtt_host, ctx->app->config.mqtt_port);
    kv(card, "client", "wtsn-configurator");
    kv(card, "topics", "tsn/#");
    lv_obj_t *c2 = row_card(scroll, "MQTT <-> OPC UA gateway");
    kv(c2, "", "Maps MQTT topics to OPC UA paths bidirectionally.");
}

/* ---- settings page ---- */
static void settings_activate(wtsn_view *self) {
    page_ctx *ctx = (page_ctx *)self->userdata;
    lv_obj_clean(g_ui_content);
    lv_obj_t *scroll = lv_obj_create(g_ui_content);
    lv_obj_set_size(scroll, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(scroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *card = row_card(scroll, "Application");
    kv(card, "db", "%s", ctx->app->config.db_path);
    kv(card, "mqtt", "%s:%d", ctx->app->config.mqtt_host, ctx->app->config.mqtt_port);
    kv(card, "opc ua", "port %u", (unsigned)ctx->app->config.opcua_port);
    kv(card, "headless", "%s", ctx->app->config.headless ? "yes" : "no");
}

static wtsn_view *make_page(wtsn_app *app, void (*act)(wtsn_view *)) {
    wtsn_view *v = calloc(1, sizeof(wtsn_view));
    page_ctx *ctx = calloc(1, sizeof(page_ctx));
    ctx->app = app;
    v->userdata = ctx;
    v->activate = act;
    return v;
}

wtsn_view *wtsn_page_devices_create(wtsn_app *app) { return make_page(app, devices_activate); }
wtsn_view *wtsn_page_tsn_create(wtsn_app *app)     { return make_page(app, tsn_activate); }
wtsn_view *wtsn_page_tas_create(wtsn_app *app)      { return make_page(app, tas_activate); }
wtsn_view *wtsn_page_vlan_create(wtsn_app *app)      { return make_page(app, vlan_activate); }
wtsn_view *wtsn_page_timesync_create(wtsn_app *app)  { return make_page(app, timesync_activate); }
wtsn_view *wtsn_page_sensors_create(wtsn_app *app)    { return make_page(app, sensors_activate); }
wtsn_view *wtsn_page_opcua_create(wtsn_app *app)      { return make_page(app, opcua_activate); }
wtsn_view *wtsn_page_mqtt_create(wtsn_app *app)       { return make_page(app, mqtt_activate); }
wtsn_view *wtsn_page_settings_create(wtsn_app *app)     { return make_page(app, settings_activate); }
