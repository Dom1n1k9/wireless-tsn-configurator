#include "mvc/controller.h"

#include <stdlib.h>
#include <string.h>

#define MAX_VIEWS 32

struct wtsn_controller {
    struct {
        wtsn_view *view;
        char name[WTSN_MAX_STR];
    } views[MAX_VIEWS];
    int num_views;
    wtsn_view *active;
};

wtsn_controller *wtsn_controller_create(void) {
    return calloc(1, sizeof(wtsn_controller));
}

void wtsn_controller_destroy(wtsn_controller *c) {
    free(c);
}

wtsn_error wtsn_controller_register_view(wtsn_controller *c, wtsn_view *v, const char *name) {
    if (!c || !v || !name || c->num_views >= MAX_VIEWS) return WTSN_ERR_INVALID_ARG;
    wtsn_strlcpy(c->views[c->num_views].name, name, WTSN_MAX_STR);
    c->views[c->num_views].view = v;
    c->num_views++;
    return WTSN_OK;
}

wtsn_view *wtsn_controller_activate(wtsn_controller *c, const char *name) {
    if (!c || !name) return NULL;
    for (int i = 0; i < c->num_views; i++) {
        if (strcmp(c->views[i].name, name) == 0) {
            if (c->active && c->active->deactivate) c->active->deactivate(c->active);
            c->active = c->views[i].view;
            if (c->active && c->active->activate) c->active->activate(c->active);
            return c->active;
        }
    }
    return NULL;
}

void wtsn_controller_route_event(wtsn_controller *c, const char *topic, void *data) {
    if (!c || !topic) return;
    if (c->active && c->active->on_event) c->active->on_event(c->active, topic, data);
}

void wtsn_controller_run(wtsn_controller *c, void *ctx) {
    (void)c;
    (void)ctx;
}
