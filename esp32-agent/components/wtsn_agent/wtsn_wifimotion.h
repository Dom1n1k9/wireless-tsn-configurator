#ifndef WTSN_WIFIMOTION_H
#define WTSN_WIFIMOTION_H

#include <stdbool.h>
#include <stdint.h>

#include "wtsn_mqtt.h"

/* "WiFiVision" — device-free coarse motion detection using the ordinary WiFi
 * link the node already has, with no extra camera/PIR hardware.
 *
 * Two complementary sources, both computed inside the WiFi task and cheaply
 * accumulated here:
 *   - Per-frame RSSI noise: the spread of the RX RSSI sampled across data
 *     packets. A moving reflector (person) makes short-term RSSI jitter grow
 *     markedly above the static-room floor.
 *   - CSI channel-variance (when CONFIG_ESP_WIFI_CSI_ENABLED is set): the
 *     average magnitude of each sub-carrier and its variance across frames.
 *     Human motion breaks the quasi-static channel, so the variance rises.
 *
 * No raw CSI is ever streamed. This module reduces every received frame to a
 * few scalars, detects threshold crossings locally, and emits only
 * motion/no-motion events ("tsn/sensors/event" + "tsn/fx/data"), the same
 * sink the wired PIR uses — so it plugs into the existing FX/actor path.
 *
 * Auto-calibration: on start the module samples a few seconds of "empty room"
 * and derives sensitivity thresholds from that floor, so it works without a
 * tuning UI. The 20 s press-to-recalibrate is not implemented; call
 * wtsn_wifimotion_recalibrate() to restart that window.
 */
void wtsn_wifimotion_init(const char *device_id, wtsn_mqtt *mq);

/* Sampling tick: called from the sensor telemetry loop (a couple times a
 * second). Reviews the short window and publishes an event when motion is
 * detected (or when it returns to rest after a busy period). */
void wtsn_wifimotion_tick(void);

/* Last known motion state (0/1), for the sensors page; NULL-safe. */
int wtsn_wifimotion_motion(void);

/* Last evaluated RSSI std-dev (dB) for telemetry/activity display. */
float wtsn_wifimotion_rssi_std(void);

/* Restart the "empty room" calibration window immediately. */
void wtsn_wifimotion_recalibrate(void);

#endif
