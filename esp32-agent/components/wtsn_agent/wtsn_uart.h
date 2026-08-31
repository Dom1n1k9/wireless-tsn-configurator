#ifndef WTSN_UART_H
#define WTSN_UART_H

#include "wtsn_mqtt.h"

/* Wired micro:bit display link over UART (no BLE).
 * micro:bit prints lines to its serial (USB or pin), ESP reads them on UART1 and
 * republishes to MQTT as micro:bit telemetry, so the webgui can show them. */

void wtsn_uart_init(wtsn_mqtt *mqtt, const char *device_id);
void wtsn_uart_start(void);

#endif
