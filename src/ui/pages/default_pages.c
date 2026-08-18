#include "ui/pages/pages.h"
#include "ui/widgets/theme.h"

#include <lvgl/lvgl.h>
#include <stdlib.h>

typedef struct {
    wtsn_app *app;
} page_ctx;

static void page_template_activate(wtsn_view *self) {
    page_ctx *ctx = (page_ctx *)self->userdata;
    (void)ctx;
    lv_obj_clean(g_ui_content);
    lv_obj_t *card = wtsn_ui_create_card(g_ui_content, "Coming soon");
    lv_obj_set_size(card, lv_pct(90), lv_pct(80));
    lv_obj_center(card);
}

static wtsn_view *make_simple_page(wtsn_app *app, void (*act)(wtsn_view *)) {
    wtsn_view *v = calloc(1, sizeof(wtsn_view));
    page_ctx *ctx = calloc(1, sizeof(page_ctx));
    ctx->app = app;
    v->userdata = ctx;
    v->activate = act ? act : page_template_activate;
    return v;
}

wtsn_view *wtsn_page_devices_create(wtsn_app *app) {
    (void)app;
    return make_simple_page(app, NULL);
}

wtsn_view *wtsn_page_tsn_create(wtsn_app *app) {
    (void)app;
    return make_simple_page(app, NULL);
}

wtsn_view *wtsn_page_vlan_create(wtsn_app *app) {
    (void)app;
    return make_simple_page(app, NULL);
}

wtsn_view *wtsn_page_timesync_create(wtsn_app *app) {
    (void)app;
    return make_simple_page(app, NULL);
}

wtsn_view *wtsn_page_opcua_create(wtsn_app *app) {
    (void)app;
    return make_simple_page(app, NULL);
}

wtsn_view *wtsn_page_mqtt_create(wtsn_app *app) {
    (void)app;
    return make_simple_page(app, NULL);
}

wtsn_view *wtsn_page_settings_create(wtsn_app *app) {
    (void)app;
    return make_simple_page(app, NULL);
}
