#include "mvc/model.h"

#include <string.h>

void wtsn_model_init(wtsn_model *m, const char *name, wtsn_event_bus *bus) {
    if (!m) return;
    wtsn_strlcpy(m->name, name, sizeof(m->name));
    m->bus = bus;
}

void wtsn_model_notify(wtsn_model *m, const char *event) {
    wtsn_model_notify_data(m, event, NULL);
}

void wtsn_model_notify_data(wtsn_model *m, const char *event, void *data) {
    if (!m || !m->bus || !event) return;
    char topic[WTSN_MAX_STR];
    wtsn_strlcpy(topic, m->name, sizeof(topic));
    wtsn_strlcpy(topic + strlen(topic), ".", sizeof(topic) - strlen(topic));
    wtsn_strlcpy(topic + strlen(topic), event, sizeof(topic) - strlen(topic));
    wtsn_event_bus_publish(m->bus, topic, data);
}
