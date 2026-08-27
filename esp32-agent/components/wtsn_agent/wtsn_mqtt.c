#include "wtsn_mqtt.h"

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
    wtsn_connected_cb conn_cb;
    void *ud;
    char device_id[32];
};

/* Subscriptions keyed to this node's own device_id so each ESP/TT device only
 * handles its own commands (previously it subscribed to tsn/cmd/+/... and any
 * node could consume another node's config). */
void wtsn_mqtt_set_device_id(wtsn_mqtt *m, const char *id) {
    if (!m) return;
    snprintf(m->device_id, sizeof(m->device_id), "%s", id ? id : "");
}

static void on_data(esp_mqtt_event_handle_t e, wtsn_mqtt *m) {
    if (m->cb && e->topic) {
        char *topic = malloc((size_t)e->topic_len + 1);
        char *payload = malloc((size_t)e->data_len + 1);
        if (topic && payload) {
            memcpy(topic, e->topic, (size_t)e->topic_len);
            topic[e->topic_len] = '\0';
            memcpy(payload, e->data, (size_t)e->data_len);
            payload[e->data_len] = '\0';
            m->cb(topic, payload, m->ud);
        }
        free(topic);
        free(payload);
    }
}

static void on_connected(esp_mqtt_event_handle_t e, wtsn_mqtt *m) {
    (void)e;
    char t[64];
    snprintf(t, sizeof(t), "tsn/cmd/%s/apply", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/qos", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/vlan", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/wifi", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/timesync", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/tas", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/stream", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/preemption", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/status", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/fx", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/actor", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/identify", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    snprintf(t, sizeof(t), "tsn/cmd/%s/ping", m->device_id[0] ? m->device_id : "+");
    esp_mqtt_client_subscribe(m->c, t, 0);
    esp_mqtt_client_subscribe(m->c, "tsn/fx/cmd/#", 0);
    ESP_LOGI(TAG, "subscribed to commands for device '%s'", m->device_id);
    if (m->conn_cb) m->conn_cb("", m->ud);
}

static void on_event(void *handler_args, esp_event_base_t base, int32_t event_id,
                    void *event_data) {
    (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
    wtsn_mqtt *m = (wtsn_mqtt *)handler_args;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        on_connected(e, m);
        break;
    case MQTT_EVENT_DATA:
        on_data(e, m);
        break;
    default:
        break;
    }
}

wtsn_mqtt *wtsn_mqtt_create(const char *host, int port, const char *client_id,
                             wtsn_cmd_cb cb, wtsn_connected_cb conn_cb, void *ud) {
    wtsn_mqtt *m = calloc(1, sizeof(wtsn_mqtt));
    if (!m) return NULL;
    m->cb = cb;
    m->conn_cb = conn_cb;
    m->ud = ud;

    esp_mqtt_client_config_t cfg = {
        .broker = { .address = { .uri = NULL, .hostname = host, .port = port,
                                 .transport = MQTT_TRANSPORT_OVER_TCP } },
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
    esp_mqtt_client_publish(m->c, topic, payload, 0, 0, 0);
}
