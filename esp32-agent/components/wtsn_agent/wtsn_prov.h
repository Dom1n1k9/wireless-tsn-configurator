#ifndef WTSN_PROV_H
#define WTSN_PROV_H

/* Start provisioning mode: bring up a SoftAP and an HTTP config portal.
   Returns when the user submits credentials (calls esp_restart afterwards)
   or immediately if NVS already has WiFi. */
void wtsn_prov_start(void);

#endif
