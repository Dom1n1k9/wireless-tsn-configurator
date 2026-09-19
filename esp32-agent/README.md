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
4. Enter WiFi SSID / password, the MQTT broker `host:port`, and the broker
   user / password (leave both empty for an anonymous broker), save.
5. The agent stores it in NVS and reboots — it now joins your WiFi and connects
   to the broker automatically.

Subsequent boots skip provisioning because the credentials are already in NVS.
To re-provision later, use the `wifi` MQTT command from the web GUI
(`tsn/cmd/<id>/wifi` with JSON `{"ssid":...,"pass":...,"mqtt":...}`).

> **Security note:** the portal is plain HTTP on an open SoftAP, so the WiFi
> password is sent in cleartext while provisioning. The SoftAP is only up for
> the few minutes of provisioning, after which it is gone. To WPA2-protect the
> setup AP, store a password in NVS (namespace `wtsn`, key `ap_pass`, min 8
> chars); the compile-time fallback is `PROV_AP_PASS_DEFAULT` in
> `shared/wtsn_prov/wtsn_prov.c`. See the main README, *Security*.

## Out-of-band configuration via NVS

You can also pre-seed settings in NVS (`idf.py menuconfig` not wired to these;
write them via a small NVS utility or the portal above):

- WiFi SSID / pass — stored in NVS under `wtsn` namespace
- MQTT host: default `wtsn-broker.local:1883` (see `wtsn_cfg.c`), override via
  portal or the `wifi` command
- Device id: auto-detected (`esp32-01` sensor board / `esp32-02` relay board),
  override with `WTSN_DEVICE_ID` or a NVS `device_id` key, or in the portal
- Optional broker auth/TLS (namespace `wtsn`): `muser`, `mpass`, `mtls` (1=on),
  `mtls_ca` (PEM), `minsec` (1 = skip verify, dev only)

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

The webgui embeds a paho-based MQTT client (see the `MqttBroker` class in
`wtsn_webgui/mqtt_broker.py`). Set the broker with env `WTSN_BROKER=host:port` or
via the FXMQTT / Settings pages. Broker auth via `WTSN_USER`/`WTSN_PASS` and TLS via
`WTSN_TLS_*` env vars; the agent-side equivalents are the NVS `muser`/`mpass`/`mtls*`
keys (see above).

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
| `tsn/cmd/<id>/stream`       | in: 802.1Qcc stream JSON (role talker/listener) |
| `tsn/cmd/<id>/preemption`   | in: `<mode>,<emac>,<pmac>`            |
| `tsn/cmd/<id>/status`       | in: empty -> replies on `tsn/status`    |
| `tsn/cmd/<id>/wifi`         | in: `{"ssid":...,"pass":...}` (pass optional -> keeps stored) |
| `tsn/cmd/<id>/ota`          | in: `{"url":"http://<host>/fw/x.bin"[,"size":N][,"crc32":"<hex>"]}` |
| `tsn/cmd/<id>/ping`         | in: `1` -> ACK with `ip` + LED blink   |
| `tsn/cmd/<id>/identify`     | in: blink LED + ACK (find-me)          |
| `tsn/cmd/<id>/actor`        | in: relay/actor command (motion actuation) |
| `tsn/cmd/<id>/reset`        | in: clear persisted TSN + WiFi state (de-provision) |
| `tsn/cmd/<id>/reboot`       | in: reboot                              |
| `tsn/cmd/<id>/factory`      | in: factory reset                       |
| `tsn/ack/<id>`              | out: `{"id","ok"[,"ip"]}`              |
| `tsn/status`               | out: JSON status (rssi, fw, ip)         |
| `tsn/discover`             | out: on connect (ip, fw, kind)          |
| `tsn/fx/cmd/#`             | in: FX / C2C commands (e.g. stream)     |
| `tsn/fx/data`              | in/out: shared FX data feed (motion, field samples) |
| `tsn/fx/<id>`              | out: per-device field exchange          |
| `tsn/sensors` / `tsn/sensors/<id>/{temp,press,hum,light,pir,...}` | out: telemetry |
| `tsn/sensors/event`        | out: PIR / WiFiVision motion events     |
| `tsn/lwt/<id>`             | retained "offline" last will           |
| `tsn/ptp`                  | out: gPTP reports                      |

## OTA with CRC verification

`tsn/cmd/<id>/ota` accepts an optional `crc32` (unsigned hex, as computed by the web
GUI firmware manager at upload time):

- The image is downloaded to the **inactive A/B partition** (`esp_https_ota`).
- Before rebooting, the agent **re-reads the target partition and verifies its CRC32**
  against the expected value (`shared/wtsn_ota`, `wtsn_ota_start_checked()`).
- **Mismatch** → the new partition is marked invalid, the previous app stays active,
  no reboot. **Match** → reboot; if the new app still fails to validate on boot, the
  bootloader rolls back automatically.

## Sensors & WiFiVision

On-board / wired sensors: **BME280** (bit-bang I2C: temp/press/hum), **TEMT6000** light,
**HC-S501 PIR** motion, **ultrasonic sonar** distance — published on `tsn/sensors`
(1 s cadence) with per-sensor history.

**WiFiVision** (`wtsn_wifimotion`) is a device-free coarse motion detector that uses the
ordinary WiFi link — no extra camera or PIR hardware:

- **RSSI noise** — the short-term spread of RX RSSI across data packets grows markedly
  when a person moves (moving reflector) above the static-room floor.
- **CSI channel variance** (with `CONFIG_ESP_WIFI_CSI_ENABLED`) — human motion breaks
  the quasi-static channel, raising per-sub-carrier magnitude variance.

No raw CSI is ever streamed: each frame is reduced to a few scalars, threshold crossings
are detected locally, and only motion/no-motion events are emitted on the **same sink as
the wired PIR** (`tsn/sensors/event` + `tsn/fx/data`) — so it plugs straight into the
existing FX / relay-actor / policy path.

## micro:bit display panel (wired UART)

The agent drives a micro:bit V2 as a local display panel over UART1 (no BLE),
both directions:

| Wire                                      | Purpose                                                            |
|-------------------------------------------|--------------------------------------------------------------------|
| micro:bit P0 (TX) --> ESP GPIO14          | micro:bit reports its own onboard sensors -> MQTT `tsn/sensors` (mb_* sensors of this node) |
| ESP GPIO15 (TX) --> micro:bit P1 (RX)     | agent pushes the node sensors once a second: `T:<C> P:<hPa> H:<%RH> L:<lx> M:<0\|1> A:<mode>` |
| GND --> GND                               | common ground (3.3V logic, no level shift needed)                  |

The micro:bit firmware (see `../microbit-sensor/`, MakeCode `main.ts` or
MicroPython `microbit_sensor.py`) shows one value with its unit at a time
(`T:27C`, `P:1006hPa`, `H:48%`, `L:918lx`, `M:1`) and holds it — **B** = next
value, **A** = previous. It **beeps through the V2 built-in speaker**
(`music.setBuiltInSpeakerEnabled(true)`) and flashes a heart when the PIR
reports motion (`M:1`, rising edge). A test tone plays at startup. No external
piezo needed (MakeCode variant).

Every UART frame is terminated with a **CRC-16/CCITT** trailer (`*XXXX`, poly
`0x1021`, init `0xFFFF`); both the ESP and the panel drop frames that fail the
check, so a corrupted wire never shows wrong values. Legacy frames without the
trailer are still accepted for compatibility.

## Limitations on ESP32

- **Real 802.1Qbv TAS, 802.1Qbu preemption, HW PTP (802.1AS)** require a
  TSN-capable MAC/PHY (e.g. some ESP32-S3 + external PHY, or an external TSN
  switch). This agent stores/applies the settings and reports them; if your board has
  HW support wire it into `wtsn_tsn.c`.
- **QoS** maps to WMM on Wi-Fi; the 802.1Q PCP bits are set on application
  frames (see `wtsn_tsn.c`).
- **Time sync** uses best-effort; real 802.1AS needs external support (e.g.
  IEEE 802.1AS-stack / gPTP on an MCU with PTP PHY).
