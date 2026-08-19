#include "ui/pages/pages.h"
#include "ui/widgets/theme.h"
#include "trace/trace.h"

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    wtsn_app *app;
    wtsn_trace *trace;
    lv_obj_t *list;
} ctx;

static lv_color_t entry_color(wtsn_trace_type type) {
    switch (type) {
    case WTSN_TRACE_COMM: return g_theme.secondary;
    case WTSN_TRACE_FRAME: return g_theme.primary;
    case WTSN_TRACE_CONFIG: return g_theme.success;
    case WTSN_TRACE_MULTICAST: return g_theme.warn;
    default: return g_theme.text_dim;
    }
}

static void rebuild_list(ctx *c) {
    lv_obj_clean(c->list);
    int n = wtsn_trace_count(c->trace);
    for (int i = 0; i < n && i < 200; i++) {
        wtsn_trace_entry *e = wtsn_trace_entry_at(c->trace, i);
        char full[WTSN_TRACE_LINE + 64];
        snprintf(full, sizeof(full), "[%s] %s: %s", e->timestamp, e->source, e->line);
        lv_obj_t *lbl = lv_label_create(c->list);
        lv_label_set_text(lbl, full);
        lv_obj_set_style_text_color(lbl, entry_color(e->type), 0);
    }
}

static void on_event(wtsn_view *self, const char *topic, void *data) {
    ctx *c = (ctx *)self->userdata;
    if (strstr(topic, "trace.entry")) {
        (void)data;
        /* refresh samozrejme na dalsom timer tik; povolime len redraw cez rebuild */
        if (c->list) rebuild_list(c);
    }
}

static void activate(wtsn_view *self) {
    ctx *c = (ctx *)self->userdata;
    lv_obj_clean(g_ui_content);
    lv_obj_t *card = wtsn_ui_create_card(g_ui_content, "Communication Trace (real + simulated, frames + config)");
    lv_obj_set_size(card, lv_pct(95), lv_pct(88));
    c->list = lv_obj_create(card);
    lv_obj_set_size(c->list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(c->list, g_theme.surface, 0);
    lv_obj_set_style_border_width(c->list, 0, 0);
    lv_obj_set_layout(c->list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(c->list, LV_FLEX_FLOW_COLUMN);
    rebuild_list(c);
}

static void deactivate(wtsn_view *self) {
    (void)self;
}

wtsn_view *wtsn_page_trace_create(wtsn_app *app) {
    wtsn_view *v = calloc(1, sizeof(wtsn_view));
    ctx *c = calloc(1, sizeof(ctx));
    c->app = app;
    c->trace = app->trace;
    v->userdata = c;
    v->activate = activate;
    v->deactivate = deactivate;
    v->on_event = on_event;
    return v;
}
