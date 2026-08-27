#ifndef WTSN_PROV_H
#define WTSN_PROV_H

/* Start provisioning mode: bring up a SoftAP and an HTTP config portal.
   Returns when the user submits credentials (calls esp_restart afterwards)
   or immediately if NVS already has WiFi. */
void wtsn_prov_start(void);

/* Start provisioning SoftAP + portal even when WiFi is already stored in NVS.
   Fallback so a device that cannot reach its saved network can be re-provisioned
   to a new network over the air (no USB/flash required). */
void wtsn_prov_start_ap(void);

#endif
