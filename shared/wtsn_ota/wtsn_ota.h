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

/* Same as wtsn_ota_start(), but after the image is written the device
 * re-reads the target partition and verifies its CRC32 against
 * `crc32_hex` (unsigned hex string as computed by the GUI at upload
 * time). On mismatch the update is aborted, the new partition is
 * marked invalid and the previous app stays active. Pass NULL to skip
 * the check. */
esp_err_t wtsn_ota_start_checked(const char *url, const char *crc32_hex);

/* Convenience for the common "url + expected size" MQTT payload form. */
esp_err_t wtsn_ota_start_sized(const char *url, size_t size);

#endif
