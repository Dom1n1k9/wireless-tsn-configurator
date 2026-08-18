#ifndef WTSN_EVENT_BUS_H
#define WTSN_EVENT_BUS_H

#include "common/common.h"

#include <stdbool.h>

typedef void (*wtsn_event_handler)(const char *topic, void *data, void *userdata);

typedef struct wtsn_event_bus wtsn_event_bus;

wtsn_event_bus *wtsn_event_bus_create(void);
void wtsn_event_bus_destroy(wtsn_event_bus *bus);
void wtsn_event_bus_publish(wtsn_event_bus *bus, const char *topic, void *data);
int wtsn_event_bus_subscribe(wtsn_event_bus *bus, const char *topic, wtsn_event_handler cb, void *userdata);

#endif
