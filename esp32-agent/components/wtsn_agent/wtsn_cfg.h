#ifndef WTSN_CFG_H
#define WTSN_CFG_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Load/save persistent config from NVS. */
bool wtsn_cfg_load(char *device_id, size_t device_id_sz,
                   char *wifi_ssid, size_t ssid_sz,
                   char *wifi_pass, size_t pass_sz,
                   char *mqtt_host, size_t host_sz,
                   int *mqtt_port);
bool wtsn_cfg_save(const char *device_id, const char *wifi_ssid,
                   const char *wifi_pass, const char *mqtt_host, int mqtt_port);
void wtsn_cfg_set_wifi(const char *ssid, const char *pass);

/* tiny substitute for the host-side str_util */
void wtsn_strlcpy(char *dst, const char *src, size_t sz);

#endif
