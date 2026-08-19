#include "ui/driver.h"

#include <lvgl/lvgl.h>
#include <SDL2/SDL.h>

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int width;
    int height;
    lv_display_t *display;
    lv_indev_t *indev;
    lv_color_t *buf1;
    lv_color_t *buf2;
} driver_state_t;

static driver_state_t g;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, lv_color_t *px) {
    (void)disp;
    SDL_UpdateTexture(g.texture, NULL, px, g.width * sizeof(lv_color_t));
    SDL_RenderClear(g.renderer);
    SDL_RenderCopy(g.renderer, g.texture, NULL, NULL);
    SDL_RenderPresent(g.renderer);
    lv_display_flush_ready(disp);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    int x, y;
    Uint32 buttons = SDL_GetMouseState(&x, &y);
    data->point.x = x;
    data->point.y = y;
    data->state = buttons & SDL_BUTTON_LMASK ? LV_INDEV_STATE_PRESSED
                                               : LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
}

int wtsn_ui_driver_init(int width, int height) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return -1;

    g.width = width;
    g.height = height;
    g.window = SDL_CreateWindow("WTSN Configurator", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, width, height,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    g.renderer = SDL_CreateRenderer(g.window, -1, SDL_RENDERER_ACCELERATED);
    if (!g.renderer) g.renderer = SDL_CreateRenderer(g.window, -1,
                                                     SDL_RENDERER_SOFTWARE);
    g.texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, width, height);

    g.buf1 = malloc(width * height * sizeof(lv_color_t));
    g.buf2 = malloc(width * height * sizeof(lv_color_t));
    g.display = lv_display_create(width, height);
    lv_display_set_buffers(g.display, g.buf1, g.buf2, width * height * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(g.display, flush_cb);

    g.indev = lv_indev_create();
    lv_indev_set_type(g.indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g.indev, read_cb);
    return 0;
}

void wtsn_ui_driver_pump(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        /* input is read on demand via SDL_GetMouseState */
        if (ev.type == SDL_QUIT) exit(0);
    }
}

void wtsn_ui_driver_deinit(void) {
    if (g.texture) SDL_DestroyTexture(g.texture);
    if (g.renderer) SDL_DestroyRenderer(g.renderer);
    if (g.window) SDL_DestroyWindow(g.window);
    SDL_Quit();
}
