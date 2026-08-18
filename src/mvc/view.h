#ifndef WTSN_VIEW_H
#define WTSN_VIEW_H

#include "common/common.h"

typedef struct wtsn_view wtsn_view;

struct wtsn_view {
    void (*activate)(wtsn_view *self);
    void (*deactivate)(wtsn_view *self);
    void (*on_event)(wtsn_view *self, const char *topic, void *data);
    void (*render)(wtsn_view *self);
    void *userdata;
};

static inline void wtsn_view_render(wtsn_view *v) {
    if (v && v->render) v->render(v);
}

#endif
