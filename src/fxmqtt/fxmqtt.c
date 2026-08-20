#include "fxmqtt/fxmqtt.h"

#include "common/log.h"
#include "common/str_util.h"

#include <stdlib.h>
#include <string.h>

wtsn_fxmqtt *wtsn_fxmqtt_create(void) {
    wtsn_fxmqtt *f = calloc(1, sizeof(wtsn_fxmqtt));
    if (!f) return NULL;
    f->server_type = WTSN_FXMQTT_SERVER_PC;
    wtsn_strlcpy(f->broker_host, "127.0.0.1", sizeof(f->broker_host));
    f->broker_port = 1883;
    return f;
}

void wtsn_fxmqtt_destroy(wtsn_fxmqtt *f) {
    free(f);
}

wtsn_error wtsn_fxmqtt_configure(wtsn_fxmqtt *f, const wtsn_fxmqtt *cfg) {
    if (!f || !cfg) return WTSN_ERR_INVALID_ARG;
    wtsn_strlcpy(f->device_id, cfg->device_id, sizeof(f->device_id));
    f->server_type = cfg->server_type;
    wtsn_strlcpy(f->broker_host, cfg->broker_host[0] ? cfg->broker_host : "127.0.0.1",
                 sizeof(f->broker_host));
    f->broker_port = cfg->broker_port > 0 ? cfg->broker_port : 1883;
    return WTSN_OK;
}

wtsn_error wtsn_fxmqtt_start(wtsn_fxmqtt *f, wtsn_mqtt_client *mqtt) {
    if (!f || !mqtt) return WTSN_ERR_INVALID_ARG;
    /* subscribe to the C2C field topics on the broker */
    wtsn_mqtt_client_subscribe(mqtt, WTSN_FXMQTT_TOPIC_FIELD);
    wtsn_mqtt_client_subscribe(mqtt, WTSN_FXMQTT_TOPIC_DATA);
    wtsn_mqtt_client_subscribe(mqtt, WTSN_FXMQTT_TOPIC_NODE);
    f->started = true;
    wtsn_log(WTSN_LOG_INFO, "fx over mqtt started server_type=%d broker %s:%d",
             (int)f->server_type, f->broker_host, f->broker_port);
    return WTSN_OK;
}

wtsn_error wtsn_fxmqtt_field_publish(wtsn_fxmqtt *f, const char *payload) {
    (void)f;
    (void)payload;
    return WTSN_OK;
}

wtsn_error wtsn_fxmqtt_send_c2c(wtsn_fxmqtt *f, const char *topic, const char *payload) {
    (void)f;
    (void)topic;
    (void)payload;
    return WTSN_OK;
}
