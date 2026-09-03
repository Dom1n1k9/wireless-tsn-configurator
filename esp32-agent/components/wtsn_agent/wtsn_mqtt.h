#ifndef WTSN_MQTT_H
#define WTSN_MQTT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct wtsn_mqtt wtsn_mqtt;

/* on_command (topic, payload) is called with a normalized topic and payload
   whenever a command arrives. */
typedef void (*wtsn_cmd_cb)(const char *topic, const char *payload, void *ud);

typedef void (*wtsn_connected_cb)(const char *client_id, void *ud);

wtsn_mqtt *wtsn_mqtt_create(const char *host, int port, const char *client_id,
                             wtsn_cmd_cb cb, wtsn_connected_cb conn_cb, void *ud);
/* Extended create with optional broker auth + TLS:
 *   user/pass   -> MQTT username/password (may be NULL for none)
 *   tls         -> use TLS over port `port` (esp_mqtt client) when non-zero
 *   tls_ca_pem  -> CA certificate / bundle to verify the broker (may be NULL)
 *   insecure    -> do not verify the broker certificate (dev only)
 * The plain wtsn_mqtt_create() calls this with NULL/no-TLS. */
wtsn_mqtt *wtsn_mqtt_create_auth(const char *host, int port, const char *client_id,
                                 const char *user, const char *pass,
                                 bool tls, const char *tls_ca_pem, bool insecure,
                                 wtsn_cmd_cb cb, wtsn_connected_cb conn_cb, void *ud);
void wtsn_mqtt_start(wtsn_mqtt *m);
void wtsn_mqtt_publish(wtsn_mqtt *m, const char *topic, const char *payload);
void wtsn_mqtt_set_device_id(wtsn_mqtt *m, const char *id);

#endif
