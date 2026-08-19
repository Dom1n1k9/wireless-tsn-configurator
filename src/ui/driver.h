#ifndef WTSN_UI_DRIVER_H
#define WTSN_UI_DRIVER_H

/* Initialise the display (window / framebuffer) and its LVGL display + input devices.
 * Returns 0 on success. Must be called after lv_init(). */
int wtsn_ui_driver_init(int width, int height);
void wtsn_ui_driver_pump(void);
void wtsn_ui_driver_deinit(void);

#endif
