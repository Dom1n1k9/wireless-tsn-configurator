#ifndef WTSN_MODEL_H
#define WTSN_MODEL_H

#include "mvc/event_bus.h"

typedef struct {
    char name[WTSN_MAX_STR];
    wtsn_event_bus *bus;
} wtsn_model;

void wtsn_model_init(wtsn_model *m, const char *name, wtsn_event_bus *bus);
void wtsn_model_notify(wtsn_model *m, const char *event);
void wtsn_model_notify_data(wtsn_model *m, const char *event, void *data);

#endif
