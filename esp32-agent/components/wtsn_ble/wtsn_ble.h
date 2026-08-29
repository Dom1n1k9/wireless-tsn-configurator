#ifndef WTSN_BLE_H
#define WTSN_BLE_H

/* Start the BLE central that connects to the micro:bit display panel and pushes
 * the node's live sensor values over the Nordic UART Service (NUS):
 *   "T:<temp> H:<hum> L:<lux> P:<motion>\n"
 *
 * Called from app_main after wifi/mqtt are up. Values are pulled from wtsn_sensor
 * getters by a background loop. */
void wtsn_ble_start(void);
void wtsn_ble_restart(void);

#endif
