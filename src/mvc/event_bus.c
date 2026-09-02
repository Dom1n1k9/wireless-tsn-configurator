#include "mvc/event_bus.h"

#include "common/str_util.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAX_SUBSCRIBERS 64

typedef struct {
    char topic[WTSN_MAX_STR];
    wtsn_event_handler cb;
    void *userdata;
} subscription;

struct wtsn_event_bus {
    subscription subs[MAX_SUBSCRIBERS];
    int num_subs;
    pthread_mutex_t lock;
};

wtsn_event_bus *wtsn_event_bus_create(void) {
    wtsn_event_bus *bus = calloc(1, sizeof(wtsn_event_bus));
    if (!bus) return NULL;
    pthread_mutex_init(&bus->lock, NULL);
    return bus;
}

void wtsn_event_bus_destroy(wtsn_event_bus *bus) {
    if (!bus) return;
    pthread_mutex_destroy(&bus->lock);
    free(bus);
}

int wtsn_event_bus_subscribe(wtsn_event_bus *bus, const char *topic,
                             wtsn_event_handler cb, void *userdata) {
    if (!bus || !topic || !cb) return WTSN_ERR_INVALID_ARG;
    pthread_mutex_lock(&bus->lock);
    if (bus->num_subs >= MAX_SUBSCRIBERS) {
        pthread_mutex_unlock(&bus->lock);
        return WTSN_ERR_BUSY;
    }
    int i = bus->num_subs++;
    wtsn_strlcpy(bus->subs[i].topic, topic, sizeof(bus->subs[i].topic));
    bus->subs[i].cb = cb;
    bus->subs[i].userdata = userdata;
    pthread_mutex_unlock(&bus->lock);
    return WTSN_OK;
}

void wtsn_event_bus_publish(wtsn_event_bus *bus, const char *topic, void *data) {
    if (!bus || !topic) return;
    /* copy subscribers list to avoid holding lock while calling callbacks */
    subscription local[MAX_SUBSCRIBERS];
    int n = 0;
    pthread_mutex_lock(&bus->lock);
    for (int i = 0; i < bus->num_subs; i++) {
        if (strncmp(topic, bus->subs[i].topic, strlen(bus->subs[i].topic)) == 0 ||
            strcmp(bus->subs[i].topic, "*") == 0) {
            local[n++] = bus->subs[i];
        }
    }
    pthread_mutex_unlock(&bus->lock);

    for (int i = 0; i < n; i++) {
        local[i].cb(topic, data, local[i].userdata);
    }
}
