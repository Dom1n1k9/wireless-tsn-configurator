#include "mqtt/mqtt_client.h"

#include "mvc/event_bus.h"

#include "common/str_util.h"

#include "common/log.h"

#include <stdlib.h>
#include <string.h>

#include <mosquitto.h>

struct wtsn_mqtt_client {
    struct mosquitto *client;
    wtsn_event_bus *bus;
    wtsn_mqtt_message_cb msg_cb;
    void *msg_ud;
    char host[128];
    int port;
    bool connected;
};

static void on_connect(struct mosquitto *mq, void *userdata, int rc) {
    wtsn_mqtt_client *c = (wtsn_mqtt_client *)userdata;
    (void)mq;
    if (rc == 0) {
        c->connected = true;
        wtsn_log(WTSN_LOG_INFO, "mqtt connected to %s:%d", c->host, c->port);
        wtsn_event_bus_publish(c->bus, "mqtt.connected", NULL);
    } else {
        c->connected = false;
        wtsn_log(WTSN_LOG_WARN, "mqtt connect refused rc=%d", rc);
    }
}

static void on_message(struct mosquitto *mq, void *userdata, const struct mosquitto_message *msg) {
    wtsn_mqtt_client *c = (wtsn_mqtt_client *)userdata;
    (void)mq;
    const char *topic = msg->topic ? msg->topic : "";
    const char *payload = msg->payload ? (const char *)msg->payload : "";
    size_t len = msg->payloadlen;

    if (c->msg_cb) {
        char *copy = malloc(len + 1);
        if (copy) {
            memcpy(copy, payload, len);
            copy[len] = '\0';
            c->msg_cb(topic, copy, len, c->msg_ud);
            free(copy);
        }
    }
    wtsn_event_bus_publish(c->bus, "mqtt.message", (void *)topic);
}

static void on_disconnect(struct mosquitto *mq, void *userdata, int rc) {
    wtsn_mqtt_client *c = (wtsn_mqtt_client *)userdata;
    (void)mq;
    c->connected = false;
    wtsn_log(WTSN_LOG_INFO, "mqtt disconnected rc=%d", rc);
    wtsn_event_bus_publish(c->bus, "mqtt.disconnected", NULL);
}

wtsn_mqtt_client *wtsn_mqtt_client_create(wtsn_event_bus *bus) {
    wtsn_mqtt_client *c = calloc(1, sizeof(wtsn_mqtt_client));
    if (!c) return NULL;
    c->bus = bus;
    return c;
}

void wtsn_mqtt_client_destroy(wtsn_mqtt_client *c) {
    if (!c) return;
    if (c->client) {
        wtsn_mqtt_client_loop_stop(c);
        mosquitto_destroy(c->client);
    }
    free(c);
}

wtsn_error wtsn_mqtt_client_connect(wtsn_mqtt_client *c, const char *host, int port,
                                    const char *client_id, const char *username, const char *password) {
    if (!c || !host || port <= 0) return WTSN_ERR_INVALID_ARG;
    if (c->client) return WTSN_ERR_BUSY;

    mosquitto_lib_init();
    c->client = mosquitto_new(client_id ? client_id : "wtsn-configurator", true, c);
    if (!c->client) return WTSN_ERR_NO_MEMORY;

    if (username) mosquitto_username_pw_set(c->client, username, password);
    mosquitto_connect_callback_set(c->client, on_connect);
    mosquitto_message_callback_set(c->client, on_message);
    mosquitto_disconnect_callback_set(c->client, on_disconnect);

    wtsn_strlcpy(c->host, host, sizeof(c->host));
    c->port = port;

    if (mosquitto_connect_async(c->client, host, port, 30) != MOSQ_ERR_SUCCESS) {
        wtsn_log(WTSN_LOG_ERROR, "mqtt connect_async failed");
        return WTSN_ERR_NET;
    }
    return WTSN_OK;
}

void wtsn_mqtt_client_disconnect(wtsn_mqtt_client *c) {
    if (c && c->client) mosquitto_disconnect(c->client);
}

wtsn_error wtsn_mqtt_client_subscribe(wtsn_mqtt_client *c, const char *topic) {
    if (!c || !c->client || !topic) return WTSN_ERR_INVALID_ARG;
    return mosquitto_subscribe(c->client, NULL, topic, 0) == MOSQ_ERR_SUCCESS
        ? WTSN_OK : WTSN_ERR_NET;
}

wtsn_error wtsn_mqtt_client_publish(wtsn_mqtt_client *c, const char *topic, const char *payload) {
    if (!c || !c->client || !topic) return WTSN_ERR_INVALID_ARG;
    return mosquitto_publish(c->client, NULL, topic, (int)strlen(payload),
                            payload, 0, false) == MOSQ_ERR_SUCCESS
        ? WTSN_OK : WTSN_ERR_NET;
}

void wtsn_mqtt_client_set_message_cb(wtsn_mqtt_client *c, wtsn_mqtt_message_cb cb, void *ud) {
    if (!c) return;
    c->msg_cb = cb;
    c->msg_ud = ud;
}

void wtsn_mqtt_client_loop_start(wtsn_mqtt_client *c) {
    if (c && c->client) mosquitto_loop_start(c->client);
}

void wtsn_mqtt_client_loop_stop(wtsn_mqtt_client *c) {
    if (c && c->client) mosquitto_loop_stop(c->client, true);
}
