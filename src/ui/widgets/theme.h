#ifndef WTSN_UI_THEME_H
#define WTSN_UI_THEME_H

#include "app/app.h"
#include <lvgl/lvgl.h>

typedef struct {
    lv_color_t bg;
    lv_color_t surface;
    lv_color_t primary;
    lv_color_t secondary;
    lv_color_t text;
    lv_color_t text_dim;
    lv_color_t success;
    lv_color_t warn;
    lv_color_t error;
    lv_color_t border;
} wtsn_theme;

extern wtsn_theme g_theme;

void wtsn_theme_apply(void);
void wtsn_theme_card(lv_obj_t *obj);
lv_obj_t *wtsn_ui_create_card(lv_obj_t *parent, const char *title);

extern lv_obj_t *g_ui_content;

#endif
