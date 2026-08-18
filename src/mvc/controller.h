#ifndef WTSN_CONTROLLER_H
#define WTSN_CONTROLLER_H

#include "common/common.h"
#include "mvc/view.h"

typedef struct wtsn_controller wtsn_controller;

typedef void (*wtsn_controller_views_ref)(wtsn_view *views[], size_t *n);

wtsn_controller *wtsn_controller_create(void);
void wtsn_controller_destroy(wtsn_controller *c);
wtsn_error wtsn_controller_register_view(wtsn_controller *c, wtsn_view *v, const char *name);
wtsn_view *wtsn_controller_activate(wtsn_controller *c, const char *name);
void wtsn_controller_route_event(wtsn_controller *c, const char *topic, void *data);
void wtsn_controller_run(wtsn_controller *c, void *ctx);

#endif
