#include "ui/widgets/theme.h"
#include <lvgl/lvgl.h>

wtsn_theme g_theme;
lv_obj_t *g_ui_content = NULL;

void wtsn_theme_apply(void) {
    g_theme.bg = lv_color_hex(0x12121A);
    g_theme.surface = lv_color_hex(0x1E1E28);
    g_theme.primary = lv_color_hex(0x4C70FF);
    g_theme.secondary = lv_color_hex(0x2f9ee6);
    g_theme.text = lv_color_hex(0xE8E8F0);
    g_theme.text_dim = lv_color_hex(0x9A9AA8);
    g_theme.success = lv_color_hex(0x31C96B);
    g_theme.warn = lv_color_hex(0xFFC94A);
    g_theme.error = lv_color_hex(0xFF5F56);
    g_theme.border = lv_color_hex(0x33333F);
}

void wtsn_theme_card(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, g_theme.surface, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, g_theme.border, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
}

lv_obj_t *wtsn_ui_create_card(lv_obj_t *parent, const char *title) {
    lv_obj_t *card = lv_obj_create(parent);
    wtsn_theme_card(card);
    lv_obj_set_size(card, lv_pct(45), LV_SIZE_CONTENT);

    if (title) {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, title);
        lv_obj_set_style_text_color(lbl, g_theme.text, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    return card;
}
