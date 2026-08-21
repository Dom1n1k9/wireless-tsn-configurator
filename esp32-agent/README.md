# ESP32 WTSN Agent (ESP-IDF)

Firmware agent for a physical **ESP32** node used by the WTSN Configurator. It
connects to the MQTT broker, subscribes to command topics, applies TSN
QoS / VLAN / time-sync / TAS / preemption settings, participates in FX over MQTT,
and reports status.

> This is the real-device counterpart to the host `tsn-node-agent` and to the
> `webgui.py` simulator. Out of the box it requires an MQTT broker (e.g.
> mosquitto) reachable from the ESP32.

## Prerequisites

- ESP-IDF v5.x (`export.sh` / `export.ps1` sourced)
- ESP32 board (DevKit recommended)
- MQTT broker reachable from the board

## Wiring / first-time configuration

The agent reads its settings from NVS. The defaults are compiled in:

- WiFi SSID / pass: edit `main` (see `wifi_init`) or provision via NVS
- MQTT host: `192.168.1.100:1883` (see `wtsn_cfg.c`), override with
  `idf.py menuconfig` if you add Kconfig, or edit `wtsn_cfg.c`.
- Device id: `esp32-01` (edit `g_device_id` in `main/main.c`)

## Build & flash

```bash
cd esp32-agent
idf.py set-target esp32        # or esp32s3, esp32c3 ...
idf.py menuconfig              # optional: set WiFi + MQTT broker
idf.py build -p /dev/ttyUSB0 flash monitor
```

## MQTT protocol

The ESP32 agent subscribes to `tsn/cmd/<device_id>/<command>` and publishes
status / FX on:

| Topic                        | Direction / purpose                  |
|------------------------------|------------------------------------|
| `tsn/cmd/<id>/qos`          | in: `<priority>` (0-7)           |
| `tsn/cmd/<id>/vlan`         | in: `<vlan_id>`                  |
| `tsn/cmd/<id>/timesync`     | in: `<mode>` (0-3)              |
| `tsn/cmd/<id>/tas`          | in: `<cycle_ns>`                 |
| `tsn/cmd/<id>/preemption`   | in: `<mode>,<emac>,<pmac>`      |
| `tsn/cmd/<id>/status`       | in: empty -> replies on `tsn/status` |
| `tsn/cmd/<id>/fx`           | in: paylaod replicated to `tsn/fx/<id>` |
| `tsn/status`                | out: JSON status                  |
| `tsn/fx/#`                  | FX / C2C field exchange           |

## Limitations on ESP32

- **Real 802.1Qbv TAS, 802.1Qbu preemption, HW PTP (802.1AS)** require a
  TSN-capable MAC/PHY (e.g. some ESP32-S3 + external PHY, or an external TSN
  switch). This agent stores/applies the settings and reports them; if your board has
  HW support wire it into `wtsn_tsn.c`.
- **QoS** maps to WMM on Wi-Fi; the 802.1Q PCP bits are set on application
  frames (see `wtsn_tsn.c`).
- **Time sync** uses best-effort; real 802.1AS needs external support (e.g.
  IEEE 802.1AS-stack / gPTP on an MCU with PTP PHY).
