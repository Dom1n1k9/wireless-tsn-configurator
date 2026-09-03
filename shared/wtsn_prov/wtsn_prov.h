#ifndef WTSN_PROV_H
#define WTSN_PROV_H

#include <stdbool.h>
#include <stddef.h>

/* Shared provisioning portal: WTSN-Setup SoftAP + HTTP config page.
 * Used by both esp32-agent and esp32-cam so the provisioning UX, NVS
 * credential model and portal stay identical across firmware.
 *
 * NOTE: the portal is plain HTTP (no TLS) and the SoftAP is open by default,
 * which is normal for a provisioning-only AP, but the WiFi password is
 * submitted in cleartext. Only use it on a trusted local network.
 * To lock the SoftAP behind WPA2-PSK, store a password in NVS under the key
 * "ap_pass" (namespace "wtsn") or change PROV_AP_PASS_DEFAULT in
 * wtsn_prov.c. The portal then can only be opened after joining the secured AP.
 */

/* Called when the user submits credentials from the portal. devid may be
 * empty (keep the existing device id). Called right before a reboot. */
typedef void (*wtsn_prov_save_fn)(const char *ssid, const char *pass,
                                  const char *devid, const char *mqtt_host);
/* Fill `out` with the device id used to prefill the portal and to derive a
 * per-board SoftAP SSID. Return false if there is no id. */
typedef bool (*wtsn_prov_load_id_fn)(char *out, size_t out_sz);

/* Configure the shared portal. Call once at boot, before wtsn_prov_start()
 * or wtsn_prov_start_ap(). */
void wtsn_prov_init(const char *portal_title, const char *default_mqtt,
                    wtsn_prov_save_fn save_cb, wtsn_prov_load_id_fn load_id_cb);

/* Start provisioning mode: bring up a SoftAP and an HTTP config portal.
 * No-op if NVS already has WiFi. */
void wtsn_prov_start(void);

/* Start the provisioning SoftAP + portal even when WiFi is already stored in
 * NVS (fallback re-provisioning; the STA stack must already be initialised). */
void wtsn_prov_start_ap(void);

/* True if a WiFi SSID is already stored in NVS namespace "wtsn". */
bool wtsn_prov_have_wifi(void);

#endif
