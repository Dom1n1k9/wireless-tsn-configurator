#include "wtsn_mqtt.h"
#include "wtsn_agent.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static const char *TAG = "mqtt";

struct wtsn_mqtt {
    esp_mqtt_client_handle_t c;
    wtsn_cmd_cb cb;
    void *ud;
};

static void on_data(esp_mqtt_event_handle_t e) {
    wtsn_mqtt *m = (wtsn_mqtt *)e->user_context;
    if (m->cb && e->topic) {
        char *payload = malloc(e->payload_len + 1);
        if (payload) {
            memcpy(payload, e->payload, e->payload_len);
            payload[e->payload_len] = '\0';
            m->cb(e->topic, payload, m->ud);
            free(payload);
        }
    }
}

static void on_connected(esp_mqtt_event_handle_t e) {
    wtsn_mqtt *m = (wtsn_mqtt *)e->user_context;
    esp_mqtt_client_subscribe(m->c, "tsn/cmd/+/qos", 0);
    esp_mqtt_client_subscribe(m->c, "tsn/cmd/+/vlan", 0);
    esp_mqtt_client_subscribe(m->c, "tsn/cmd/+/timesync", 0);
    esp_mqtt_client_subscribe(m->c, "tsn/cmd/+/tas", 0);
    esp_mqtt_client_subscribe(m->c, "tsn/cmd/+/preemption", 0);
    esp_mqtt_client_subscribe(m->c, "tsn/cmd/+/status", 0);
    esp_mqtt_client_subscribe(m->c, "tsn/cmd/+/fx", 0);
    esp_mqtt_client_subscribe(m->c, "tsn/fx/#", 0);
    ESP_LOGI(TAG, "subscribed to commands");
}

static void on_event(void *handler_args, esp_event_base_t base, int32_t event_id,
                    void *event_data) {
    (void)base; (void)handler_args;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
    wtsn_mqtt *m = (wtsn_mqtt *)e->user_context;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        on_connected(e);
        break;
    case MQTT_EVENT_DATA:
        on_data(e);
        break;
    default:
        break;
    }
}

wtsn_mqtt *wtsn_mqtt_create(const char *host, int port, const char *client_id,
                             wtsn_cmd_cb cb, void *ud) {
    wtsn_mqtt *m = calloc(1, sizeof(wtsn_mqtt));
    if (!m) return NULL;
    m->cb = cb;
    m->ud = ud;

    esp_mqtt_client_config_t cfg = {
        .broker = { .address = { .uri = NULL, .hostname = host, .port = port } },
        .credentials = { .client_id = client_id },
        .session = { .keepalive = 30 },
    };
    m->c = esp_mqtt_client_init(&cfg);
    if (!m->c) { free(m); return NULL; }
    esp_mqtt_client_register_event(m->c, ESP_EVENT_ANY_ID, on_event, m);
    return m;
}

void wtsn_mqtt_start(wtsn_mqtt *m) {
    if (!m) return;
    esp_mqtt_client_start(m->c);
}

void wtsn_mqtt_publish(wtsn_mqtt *m, const char *topic, const char *payload) {
    if (!m || !m->c) return;
    esp_mqtt_client_publish(m->c, topic, payload, 0, 1, 0);
}
