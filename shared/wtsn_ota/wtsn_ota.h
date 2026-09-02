#ifndef WTSN_OTA_H
#define WTSN_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Shared OTA update for esp32-agent and esp32-cam.
 *
 * Downloads a firmware image from an HTTP(S) URL and installs it to the
 * other OTA partition (A/B). The new app boots with a rollback window:
 * if it fails to call esp_ota_mark_app_valid_cancel_rollback() within
 * OTA_APP_ROLLBACK_TIMEOUT seconds, the bootloader automatically rolls
 * back to the previous partition.
 *
 * The device reboots on success. NOTE: plain HTTP URLs are accepted
 * (CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y) so the web GUI can serve the image;
 * use an HTTPS URL when the update path is not on a trusted LAN.
 */

/* Start an OTA update from `url` in a new task. Returns ESP_OK if the
 * task was started (the download itself may still fail), ESP_FAIL on
 * alloc errors. Safe to call from an MQTT command callback. */
esp_err_t wtsn_ota_start(const char *url);

/* Convenience for the common "url + expected size" MQTT payload form. */
esp_err_t wtsn_ota_start_sized(const char *url, size_t size);

#endif
