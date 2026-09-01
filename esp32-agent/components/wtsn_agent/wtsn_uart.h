#ifndef WTSN_UART_H
#define WTSN_UART_H

#include "wtsn_mqtt.h"

/* Wired micro:bit display link over UART (no BLE), two directions:
 *  - RX (GPIO14 <- micro:bit P0): micro:bit prints its own sensor lines, the ESP
 *    republishes them to MQTT as micro:bit telemetry so the webgui can show them.
 *  - TX (GPIO15 -> micro:bit P1): the ESP pushes the node sensor values once a
 *    second ("T:<C> P:<hPa> H:<%RH> L:<lx> M:<0|1> A:<mode>") so the micro:bit
 *    can display them and beep when the PIR sees motion. */

void wtsn_uart_init(wtsn_mqtt *mqtt, const char *device_id);
void wtsn_uart_start(void);

/* Send one line to the micro:bit (no \n appended by this function). */
void wtsn_uart_send_line(const char *line);

#endif
