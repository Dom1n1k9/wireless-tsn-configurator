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
void wtsn_cfg_set_device_id(const char *device_id);
bool wtsn_cfg_load_device_id(char *out, size_t *sz);

/* Persist the applied TSN configuration so it survives a restart. */
bool wtsn_cfg_save_tsn_state(const int priority, const int traffic_class,
                             const int vlan_id, const int preemption,
                             const int timesync_mode, const int64_t tas_cycle_ns,
                             const int *gates, const int64_t *durations,
                             const int gcl_entries);
bool wtsn_cfg_load_tsn_state(int *priority, int *traffic_class, int *vlan_id,
                             int *preemption, int *timesync_mode, int64_t *tas_cycle_ns,
                             int *gates, int64_t *durations, int *gcl_entries);

/* tiny substitute for the host-side str_util */
void wtsn_strlcpy(char *dst, const char *src, size_t sz);

#endif
