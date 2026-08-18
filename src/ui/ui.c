#include "ui/ui.h"
#include "ui/pages/pages.h"
#include "ui/widgets/theme.h"

#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    wtsn_app *app;
    wtsn_controller *controller;
    lv_obj_t *root;
} ui_state;

static ui_state g_ui;

static void on_event(wtsn_view *self, const char *topic, void *data) {
    (void)self;
    (void)topic;
    (void)data;
}

static void render(wtsn_view *self) {
    (void)self;
}

static void nav_click(lv_event_t *e) {
    const char *page = lv_event_get_user_data(e);
    wtsn_controller_activate(g_ui.controller, page);
}

static void build_nav(lv_obj_t *parent) {
    static const char *pages[] = { "dashboard", "devices", "tsn", "vlan",
                                   "timesync", "opcua", "mqtt", "settings", NULL };
    for (int i = 0; pages[i]; i++) {
        lv_obj_t *btn = lv_button_create(parent);
        lv_obj_set_size(btn, lv_pct(100), 40);
        lv_obj_set_style_bg_color(btn, g_theme.surface, 0);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, pages[i]);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, nav_click, LV_EVENT_CLICKED, (void *)pages[i]);
    }
}

int wtsn_ui_run(wtsn_app *app) {
    lv_init();
    wtsn_theme_apply();

    g_ui.app = app;
    g_ui.controller = wtsn_controller_create();

    g_ui.root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(g_ui.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_ui.root, g_theme.bg, 0);

    lv_obj_t *sidebar = lv_obj_create(g_ui.root);
    lv_obj_set_size(sidebar, 200, lv_pct(100));
    lv_obj_set_style_bg_color(sidebar, g_theme.surface, 0);
    lv_obj_set_style_border_width(sidebar, 0, 0);
    build_nav(sidebar);

    g_ui_content = lv_obj_create(g_ui.root);
    lv_obj_set_pos(g_ui_content, 200, 0);
    lv_obj_set_size(g_ui_content, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_ui_content, g_theme.bg, 0);
    lv_obj_set_style_border_width(g_ui_content, 0, 0);
    lv_obj_set_style_pad_all(g_ui_content, 16, 0);
    lv_obj_set_layout(g_ui_content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(g_ui_content, LV_FLEX_FLOW_COLUMN);

    wtsn_controller_register_view(g_ui.controller, wtsn_page_dashboard_create(app), "dashboard");
    wtsn_controller_register_view(g_ui.controller, wtsn_page_devices_create(app), "devices");
    wtsn_controller_register_view(g_ui.controller, wtsn_page_tsn_create(app), "tsn");
    wtsn_controller_register_view(g_ui.controller, wtsn_page_vlan_create(app), "vlan");
    wtsn_controller_register_view(g_ui.controller, wtsn_page_timesync_create(app), "timesync");
    wtsn_controller_register_view(g_ui.controller, wtsn_page_opcua_create(app), "opcua");
    wtsn_controller_register_view(g_ui.controller, wtsn_page_mqtt_create(app), "mqtt");
    wtsn_controller_register_view(g_ui.controller, wtsn_page_settings_create(app), "settings");

    wtsn_view *v = wtsn_controller_activate(g_ui.controller, "dashboard");
    if (v && v->activate) v->activate(v);

    while (1) {
        lv_timer_handler();
        lv_delay_ms(16);
    }
    return 0;
}
