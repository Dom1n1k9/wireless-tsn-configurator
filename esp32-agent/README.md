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
idf.py menuconfig             # optional: set WiFi + MQTT broker (or via NVS)
idf.py build -p /dev/ttyUSB0 flash monitor
```

## Wiring to the web GUI (real mode)

1. Set an MQTT broker reachable from both the PC and the ESP32 (e.g. mosquitto
   on the PC, listening on the LAN interface).
2. On the **FXMQTT** page set broker to `<PC-IP>:1883` and choose a node/PC as
   Field Server, Save.
3. Switch the top-right mode to **Real**.
4. Click **"Execute settings on controller"** (bottom bar) — the webgui now
   connects to the broker and publishes a **single JSON snapshot** on
   `tsn/cmd/<device>/apply` for every configured device, exactly what this ESP32
   agent consumes, plus `status`. The agent replies on `tsn/ack` and
   `tsn/status`; the webgui subscribes to these so devices show online.

The webgui embeds a stdlib-only MQTT 3.1.1 client (no paho needed), see the
`MqttBroker` class in `webgui.py`. Set broker with env `WTSN_BROKER=host:port`
or via the FXMQTT / Settings pages. Broker auth via `WTSN_USER`/`WTSN_PASS`
env vars.

**Note:** in simulation mode nothing is published — it stays a pure in-browser/DB
demo. Real commands only go out in **real** mode.

## MQTT protocol

The ESP32 agent subscribes to `tsn/cmd/<device_id>/<command>` and publishes
status / ack / FX on:

| Topic                        | Direction / purpose                        |
|------------------------------|------------------------------------------|
| `tsn/cmd/<id>/apply`        | in: JSON snapshot (preferred)             |
| `tsn/cmd/<id>/qos`          | in: `<priority>` (0-7)                 |
| `tsn/cmd/<id>/vlan`         | in: `<vlan_id>`                        |
| `tsn/cmd/<id>/timesync`     | in: `<mode>` (0-3)                    |
| `tsn/cmd/<id>/tas`          | in: `<cycle_ns>`                       |
| `tsn/cmd/<id>/preemption`   | in: `<mode>,<emac>,<pmac>`            |
| `tsn/cmd/<id>/status`       | in: empty -> replies on `tsn/status`    |
| `tsn/ack`                  | out: `{"id","ok"}`                     |
| `tsn/status`               | out: JSON status                        |
| `tsn/discover`             | out: on connect                        |
| `tsn/fx/#`                | FX / C2C field exchange                 |
| `tsn/fx/field`            | in/out: C2C field exchange             |
| `tsn/fx/<id>`             | in/out: device field exchange           |

## Limitations on ESP32

- **Real 802.1Qbv TAS, 802.1Qbu preemption, HW PTP (802.1AS)** require a
  TSN-capable MAC/PHY (e.g. some ESP32-S3 + external PHY, or an external TSN
  switch). This agent stores/applies the settings and reports them; if your board has
  HW support wire it into `wtsn_tsn.c`.
- **QoS** maps to WMM on Wi-Fi; the 802.1Q PCP bits are set on application
  frames (see `wtsn_tsn.c`).
- **Time sync** uses best-effort; real 802.1AS needs external support (e.g.
  IEEE 802.1AS-stack / gPTP on an MCU with PTP PHY).
