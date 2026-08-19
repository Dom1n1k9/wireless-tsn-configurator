#include "ui/pages/pages.h"
#include "ui/widgets/theme.h"
#include "device/device.h"
#include "db/db_vlan.h"
#include "db/db_tas.h"

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { wtsn_app *app; } ctx;

typedef struct { const char *label; char value[64]; } statct;

static void make_stat_card(lv_obj_t *parent, const char *label, const char *value,
                         lv_color_t accent) {
    lv_obj_t *card = wtsn_ui_create_card(parent, label);
    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(v, accent, 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void on_event(wtsn_view *self, const char *topic, void *data) {
    (void)data;
    (void)self;
    if (strstr(topic, "device") || strstr(topic, "trace")) {
        /* data changes; rebuild handled lazily by re-activation */
    }
}

static int count_vlan_cb(const wtsn_vlan_group *g, void *ud) {
    (void)g; (*(int *)ud)++; return 0;
}

static int count_tas_cb(const wtsn_tas_schedule *s, void *ud) {
    (void)s; (*(int *)ud)++; return 0;
}

typedef struct { int online; int total; } devstat;
static void dev_cb(const wtsn_device *d, void *ud) {
    devstat *ds = (devstat *)ud;
    ds->total++;
    if (d->status == WTSN_DEVICE_ONLINE) ds->online++;
}

static void dashboard_activate(wtsn_view *self) {
    ctx *c = (ctx *)self->userdata;
    lv_obj_clean(g_ui_content);

    devstat ds = {0, 0};
    if (c->app->devices) wtsn_device_manager_for_each(c->app->devices, dev_cb, &ds);

    int vc = 0, tc = 0;
    if (c->app->db.handle) {
        wtsn_db_vlan_group_for_each(&c->app->db, count_vlan_cb, &vc);
        wtsn_db_tas_for_each(&c->app->db, count_tas_cb, &tc);
    }

    lv_obj_t *row = lv_obj_create(g_ui_content);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);

    char b[64];
    snprintf(b, sizeof(b), "%d", ds.online);
    make_stat_card(row, "Devices Online", b, g_theme.success);
    snprintf(b, sizeof(b), "%d / %d", vc, tc);
    make_stat_card(row, "VLAN / TAS Schedules", b, g_theme.secondary);
    snprintf(b, sizeof(b), "port %u · ns %d", (unsigned)wtsn_opcua_server_port(c->app->opcua),
             wtsn_opcua_server_ns(c->app->opcua));
    make_stat_card(row, "OPC UA Server", b, g_theme.primary);

    lv_obj_t *info = wtsn_ui_create_card(g_ui_content, "Configuration pushed to nodes");
    lv_obj_set_size(info, lv_pct(100), LV_SIZE_CONTENT);
    lv_label_t *lbl = lv_label_create(info);
    lv_label_set_text(lbl, "QoS 802.1Q · VLAN · 802.1AS gPTP · 802.1Qbv TAS/GCL · "
                          "Frame Preemption 802.1Qbu · Sensors · MQTT · OPC UA PubSub (FX)");
    lv_obj_set_style_text_color((lv_obj_t *)lbl, g_theme.text_dim, 0);
}

wtsn_view *wtsn_page_dashboard_create(wtsn_app *app) {
    wtsn_view *v = calloc(1, sizeof(wtsn_view));
    ctx *c = calloc(1, sizeof(ctx));
    c->app = app;
    v->userdata = c;
    v->activate = dashboard_activate;
    v->on_event = on_event;
    return v;
}
