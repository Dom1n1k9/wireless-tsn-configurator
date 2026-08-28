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

## First-time configuration (provisioning portal)

On first boot (no WiFi credentials in NVS) the agent enters **provisioning mode**:
it starts a SoftAP named **`WTSN-Setup`** and serves a small configuration
portal.

1. Power on the ESP32 — it broadcasts `WTSN-Setup`.
2. Connect your phone/PC to that SoftAP.
3. Open **http://192.168.4.1/** in a browser.
4. Enter your WiFi SSID / password and the MQTT broker host, save.
5. The agent stores it in NVS and reboots — it now joins your WiFi and connects
   to the broker automatically.

Subsequent boots skip provisioning because the credentials are already in NVS.
To re-provision later, use the `wifi` MQTT command from the web GUI
(`tsn/cmd/<id>/wifi` with JSON `{"ssid":...,"pass":...,"mqtt":...}`).

## Out-of-band configuration via NVS

You can also pre-seed settings in NVS (`idf.py menuconfig` not wired to these;
write them via a small NVS utility or the portal above):

- WiFi SSID / pass — stored in NVS under `wtsn` namespace
- MQTT host: `192.168.1.10:1883` (see `wtsn_cfg.c`), override via portal
- Device id: `esp32-01` (edit `g_device_id` in `main/main.c`)

## Build & flash

```bash
cd esp32-agent
idf.py set-target esp32        # or esp32s3, esp32c3 ...
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
   agent consumes, plus `status`. The agent replies on `tsn/ack/<id>` and
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
| `tsn/ack/<id>`              | out: `{"id","ok"}`                     |
| `tsn/status`               | out: JSON status                        |
| `tsn/discover`             | out: on connect                        |
| `tsn/fx/#`                | FX / C2C field exchange                 |
| `tsn/fx/field`            | in/out: C2C field exchange             |
| `tsn/fx/<id>`             | in/out: device field exchange           |
| `tsn/sensors/<id>/{temp,press,hum,light,pir}` | out: per-sensor readable topics |

## Limitations on ESP32

- **Real 802.1Qbv TAS, 802.1Qbu preemption, HW PTP (802.1AS)** require a
  TSN-capable MAC/PHY (e.g. some ESP32-S3 + external PHY, or an external TSN
  switch). This agent stores/applies the settings and reports them; if your board has
  HW support wire it into `wtsn_tsn.c`.
- **QoS** maps to WMM on Wi-Fi; the 802.1Q PCP bits are set on application
  frames (see `wtsn_tsn.c`).
- **Time sync** uses best-effort; real 802.1AS needs external support (e.g.
  IEEE 802.1AS-stack / gPTP on an MCU with PTP PHY).
