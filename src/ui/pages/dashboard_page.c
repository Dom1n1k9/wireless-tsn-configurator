#include "ui/pages/pages.h"
#include "ui/widgets/theme.h"

#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    wtsn_app *app;
} ctx;

static void on_event(wtsn_view *self, const char *topic, void *data) {
    (void)self;
    (void)topic;
    (void)data;
}

static void dashboard_activate(wtsn_view *self) {
    ctx *c = (ctx *)self->userdata;
    (void)c;
    lv_obj_clean(g_ui_content);

    static const char *cards[] = { "Devices Online", "VLAN Groups",
                                   "Active Schedules", "Gateways", NULL };
    lv_obj_t *row = lv_obj_create(g_ui_content);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_color(row, lv_color_transparent(), 0);
    lv_obj_set_style_border_width(row, 0, 0);

    char buf[128];
    for (int i = 0; cards[i]; i++) {
        lv_obj_t *card = wtsn_ui_create_card(row, cards[i]);
        lv_obj_set_size(card, lv_pct(20), 90);
        lv_obj_set_style_pad_top(card, 40, 0);
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, "--");
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        (void)buf;
    }

    lv_obj_t *status = wtsn_ui_create_card(g_ui_content, "System Status");
    lv_obj_set_size(status, lv_pct(90), lv_pct(40));
    lv_obj_t *txt = lv_label_create(status);
    lv_label_set_text(txt, "OPC UA server and MQTT bridge active.");
    lv_obj_set_style_text_color(txt, g_theme.text_dim, 0);
    lv_obj_center(txt);
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
