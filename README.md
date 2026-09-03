# Wireless TSN Configurator

> **Central controller for Wireless TSN** — ESP32, Raspberry Pi, STM32, NXP and
> Linux inline with IEEE 802.1Qcc. **Zero-touch node onboarding**: flash the
> agent, connect it to power, and it provisions itself.

A production-grade application for the configuration and control of **Wireless Time
Sensitive Networking (W-TSN)** nodes — ESP32, Raspberry Pi, STM32, NXP, Linux and
other microcontroller platforms. The **control-plane core is written in pure C (C11)**,
and the front-end is a **Python web GUI** (`webgui.py`).

It acts as a **centralized controller (CNC-style control plane, aligned with IEEE
802.1Qcc)** that discovers wireless nodes, manages them, applies QoS / VLAN /
time-synchronization / schedule policies, reads sensors, and exposes the whole network over
**FXMQTT** — OPC UA FX / C2C Field Exchange carried over MQTT. It ships a
firmware agent for physical devices, a generic node simulator for virtual ones, and a live
communication trace.

---

## Quick start

Everything runs from one machine. See [Setup guide](#setup-guide-initialization-step-by-step)
for a physical **ESP32** node.

```bash
# host dependencies (Linux)
sudo apt update
sudo apt install -y build-essential cmake libsqlite3-dev libmosquitto-dev mosquitto python3 python3-pip

# python package for the web GUI (MQTT client)
python3 -m pip install paho-mqtt

# build (C core: CLI + tests + simulator + agent)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)

# run the tests
./build/wtsn-tests

# web GUI  ->  http://127.0.0.1:8000
python3 webgui.py
```

> **Windows:** the web GUI runs on Windows too and only needs `paho-mqtt`:
> `python -m pip install paho-mqtt`, then `python webgui.py`. The C core targets
> Linux/ESP-IDF.

---

## One-launcher script (`run.sh`)

`run.sh` launches everything needed in one go:

- **MQTT broker** (mosquitto on `0.0.0.0:1883`, auto-detects your LAN IP)
- **Web GUI** (`webgui.py`) on http://127.0.0.1:8000 and opens the browser
- **A new terminal** with the **WiFi provisioning helper** for the ESP32 — it shows
  exactly how to reach the `WTSN-Setup` SoftAP and open http://192.168.4.1/

```bash
./run.sh          # broker + GUI + browser + provisioning helper terminal
./run.sh --flash  # additionally build & flash the ESP32 agent first
./run.sh --headless  # services only (broker + GUI + mDNS), no browser/terminal
```

`run.sh` also advertises this PC as the MQTT broker via **mDNS**
(`wtsn-broker.local`) so ESP32 nodes find the broker by name. Details in
[mDNS broker discovery](#mdns-broker-discovery).

> **Auto-start at login:** the helper installs a desktop autostart entry that runs
> `run.sh --headless` so the broker + GUI + mDNS come up on login. Disable it if
> you prefer manual control. The GUI is also self-healing: a watchdog re-checks it
> every 4 s and restarts it if it wedges (log: `/tmp/wtsn_mon.log`).

**Windows:** `run.ps1` is the Windows equivalent — it detects your LAN IP, starts
mosquitto (if `mosquitto.exe` is on PATH) listening on `0.0.0.0:1883`, launches the
web GUI in the background with a health-check restart loop, and opens the browser.

```powershell
.\run.ps1            # broker + GUI + browser
.\run.ps1 -Headless  # services only, no browser
```

### Desktop launcher (double-click)

Nemo / Nautilus execute text files the wrong way, so double-clicking `run.sh` does
nothing. Instead install the **desktop shortcut** once:

```bash
./launcher/install.sh
```

Then double-click the **"WTSN Configurator"** icon on your Desktop.

### mDNS broker discovery

Instead of a hardcoded broker IP (which goes stale when this PC changes networks or gets
a new DHCP address), the firmware defaults the broker hostname to **`wtsn-broker.local`**
and `run.sh` advertises it with **avahi** — so nodes always resolve to the current IP.

```bash
sudo apt install -y avahi-daemon avahi-utils
./run.sh --headless   # or just ./run.sh
```

> If you prefer a fixed IP-based broker, ignore this — re-enter the broker address in the
> setup portal.

---

## Zero-touch onboarding

**Flash it. Power it. It connects itself.**

- On first boot a node with no WiFi credentials starts a **SoftAP `WTSN-Setup`** and
  serves a config portal at **http://192.168.4.1/** — enter your WiFi SSID /
  password and MQTT broker; it saves them to NVS, reboots, joins your network and
  announces itself to the configurator. No soldering, no serial config.
- Controllers (ESP32, later STM32 / NXP / RPi) use the **same agent protocol** —
  the host `tsn-node-agent` and embedded agents all speak one JSON `/apply` snapshot.
- WiFi can be changed later remotely via the web GUI (`wifi` command on
  `tsn/cmd/<id>/wifi`).

### Automatic re-provisioning on a lost network

If a node can no longer reach its saved network, it gives up after a few failed reconnects
and **automatically restarts the `WTSN-Setup` SoftAP + portal** so you can re-point
it over the air. Each board advertises a **unique SSID** (`WTSN-Setup-<device-id>`)
so you can tell multiple boards apart in setup mode.

### Security note: plaintext provisioning

The provisioning portal is intentional **plain HTTP on an open SoftAP** (no TLS, no
SoftAP password): the WiFi password is transmitted in cleartext from your phone/PC to
the board. This is the standard trade-off for zero-touch onboarding (ESP's own
provisioning examples do the same in their basic form), but it means:

- anyone on the `WTSN-Setup-<id>` SoftAP can read the credentials being entered;
- the portal is only reachable from that SoftAP, so the exposure window is the few
  minutes you spend provisioning, not your production network.

For sensitive deployments, keep provisioning physically supervised, or secure the
provisioning AP:

- Store a **SoftAP password** in NVS (namespace `wtsn`, key `ap_pass`, min 8 chars)
  to put `WTSN-Setup-*` behind **WPA2-PSK** (`PROV_AP_PASS_DEFAULT` in
  `shared/wtsn_prov/wtsn_prov.c` is the compile-time fallback; leave it empty for an
  open AP).
- MQTT broker access can be restricted to **username/password + TLS**: on the agent,
  store `muser` / `mpass` / `mtls` / `mtls_ca` / `minsec` in NVS (namespace `wtsn`),
  or set `WTSN_USER` / `WTSN_PASS` / `WTSN_TLS_CA` / `WTSN_TLS_CERT` / `WTSN_TLS_KEY`
  (and `WTSN_TLS_INSECURE=1` to skip verification, dev only) for the web GUI.

After provisioning the SoftAP is gone and normal operation only uses MQTT.

---

## Setup guide (initialization, step by step)

This walks you through getting a physical **ESP32 agent** online and controllable from the web
GUI. Do these once when you set up the system.

### 1. Install dependencies (host)

```bash
sudo apt update
sudo apt install -y build-essential cmake libsqlite3-dev libmosquitto-dev mosquitto python3 python3-pip
python3 -m pip install paho-mqtt
```

### 2. Set up the MQTT broker

The broker must listen on all interfaces so the ESP32 (same LAN) can reach it:

```bash
echo 'listener 1883 0.0.0.0'  | sudo tee /etc/mosquitto/conf.d/wtsn.conf
echo 'allow_anonymous true'          | sudo tee -a /etc/mosquitto/conf.d/wtsn.conf
sudo systemctl restart mosquitto
sudo ufw allow 1883/tcp            # firewall often blocks inbound MQTT
ss -tln | grep 1883               # expect LISTEN 0.0.0.0:1883
```

### 3. Provision the WiFi (ESP32)

The agent starts a SoftAP `WTSN-Setup` on first boot (no credentials) or as a
fallback if it cannot join the saved WiFi within ~15 s:

1. Connect your phone/PC to the `WTSN-Setup` SSID (no password).
2. Open **http://192.168.4.1/**.
3. Enter an optional Device ID, your WiFi SSID / password, and the broker address
   (e.g. `192.168.0.149` or `wtsn-broker.local`).
4. Save → the agent stores it in NVS, reboots, blinks, joins WiFi and announces on MQTT.

> **Note:** from a *phone/mac hotspot*, client isolation can block the node. Prefer a
> normal router WiFi.

### 4. Connect it in the GUI

1. Open **http://127.0.0.1:8000** and, in the top-right, switch **Simulation → REAL**.
2. Set the broker to `192.168.0.149:1883` (FXMQTT / Settings) and save.
3. Add a device with id **`esp32-01`** (kind *ESP32*) on the Devices page.
4. Configure TSN (QoS, VLAN, TimeSync, TAS, ...) for that device.
5. Click **Execute settings on controller** (blue button in the header). The GUI
   publishes a JSON snapshot on `tsn/cmd/esp32-01/apply`; the agent replies on
   `tsn/ack/esp32-01`.

---

## What it does

- **Automatic device discovery** via MQTT and plugins (extensible for future protocols)
- **Device management** — online / offline / error states, full info (ID, IP, firmware,
  last seen, supported TSN features)
- **Centralized configuration (IEEE 802.1Qcc aligned)** — pushes QoS / VLAN /
  time-sync / schedules to the nodes
- **IEEE 802.1Qcc TSN Stream reservation** — Talker / Listener streams with latency,
  interval, VLAN and priority; deploy to all nodes over MQTT
- **IEEE 802.1Q QoS** — priority 0–7, traffic classes, bandwidth reservation, latency
- **IEEE 802.1Q VLAN** — VLAN ID, group membership
- **Time synchronization (gPTP, IEEE 802.1AS)** — grandmaster selection, local /
  external grandmaster modes
- **Time Aware Scheduling (IEEE 802.1Qbv)** — build and edit Gate Control Lists
  (GCL), cycle time, deploy schedules, visualize gate windows (full GCL is pushed over
  `/apply`)
- **Frame Preemption (IEEE 802.1Qbu)** — express frames preempt preemptable classes
- **Persistent per-device config on the ESP32 agent** — QoS / VLAN / TimeSync /
  TAS-GCL / Preemption stored to NVS and restored on reboot
- **Sensor management** — temperature, pressure, IMU, distance, GPIO sensors with
  diagnostics, **1 h history + sparklines** in the GUI
- **Firmware OTA** — upload a `.bin` in the GUI, the configurator serves it over HTTP
  (`/fw/<file>`) and commands the device to download + flash it (A/B slots, auto
  rollback on bad boot)
- **Live camera** — ESP32-CAM nodes show their MJPEG stream inline on the Devices page
- **Instant updates** — the GUI gets state changes over a **WebSocket** (automatic
  polling fallback if the socket drops)
- **OPC UA FX over MQTT (FXMQTT)** — PubSub / C2C Field Exchange over MQTT
- **MQTT client** — the single communication channel
- **Live monitor** — a searchable, pausable network/frame trace in the GUI
- **Config backup / restore** — export the whole configuration to JSON and import it back

All state is persisted in **SQLite**. The web GUI is a **Python (mostly stdlib)**
front-end that also uses the bundled **paho-mqtt** client.

---

## Web GUI (`webgui.py`)

A self-contained single-file SPA served on http://127.0.0.1:8000:

| Page        | Purpose                                                        |
|-------------|----------------------------------------------------------------|
| Devices     | add/remove/ping nodes, status, IP, **firmware version**, **live camera (ESP32-CAM)**, TSN features; **firmware upload + OTA** |
| Monitor     | live network/frame trace with **search filter**, **Pause/Start** and Clear |
| Sensors     | live values per node + **1 h history sparklines**                 |
| FXMQTT      | Field Server / Participant, broker address                          |
| Sync        | gPTP grandmaster / slave setup                                  |
| QoS / VLAN  | priority mapping, VLAN groups and membership                       |
| TAS / GCL   | gate control lists with **visual gate windows**                   |
| Preemption  | eMAC / pMAC priority split                                    |
| Streams     | 802.1Qcc talker / listener reservations, deploy                |
| Settings    | broker, db info, **Export / Restore configuration (JSON)**      |

Run it directly:

```bash
python3 webgui.py [--host H] [--port P]
# env: WTSN_HOST, WTSN_PORT, WTSN_DB, WTSN_BROKER, WTSN_USER, WTSN_PASS,
#      WTSN_WEB_USER, WTSN_WEB_PASS
```

**Simulation mode** fabricates a stable set of nodes and sensors so everything can be
exercised without hardware. **Real mode** connects to your MQTT broker and live devices.

> **TLS** is not bundled — put the GUI behind a reverse proxy (nginx/caddy) for HTTPS.

---

## Build & run (C core)

```bash
cd wtsn-configurator
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)
./build/wtsn-tests          # run tests

# headless / CLI
./build/wtsn-cli --headless --db ./config.db

# package
cpack -G TGZ
```

### CLI options

| Flag              | Description                             |
|-------------------|-----------------------------------------|
| `--db <path>`     | SQLite database file (default `wtsn.db`) |
| `--mqtt-host <h>` | MQTT broker host                        |
| `--mqtt-port <p>` | MQTT broker port (default 1883)        |
| `--plugin-dir <d>`| directory of discovery plugins (.so)     |
| `--headless`      | CLI mode, no GUI                        |

---

## TSN streams (IEEE 802.1Qcc)

The CNC defines **Talker / Listener streams**:

- Each stream binds a **talker** to one or more **listeners** on a VLAN, with max
  latency, max interval, priority and data-frame priority.
- Streams are validated and follow an 802.1Qcc lifecycle:
  **configured → ready → (failed/standby)**.
- Deploy pushes the stream to every endpoint on `tsn/cmd/<device>/stream` (JSON
  snapshot); both the host agent (RPi/Linux) and the ESP32 agent understand it.
- Managed under **IEEE 802.1Qcc → TSN Streams** in the GUI and via
  `src/stream/` (`tsn_manager`) in the core.

---

## TSN node firmware agent

`tsn-node-agent` runs on physical devices and executes configurator commands over MQTT:

```bash
cmake --build build --target tsn-node-agent
./build/tsn-node-agent --id node-01 --platform linux --mqtt-host broker.local
```

| Option         | Description                             |
|----------------|-----------------------------------------|
| `--id`         | Device id reported to the configurator    |
| `--platform`   | `linux`, `raspberry_pi`, `esp32`, `stm32`, `nxp` |
| `--mqtt-host`  | Configurator / broker host             |
| `--mqtt-port`  | MQTT port (default 1883)          |

Commands on `tsn/cmd/<device>`: `apply` (preferred — full JSON snapshot), `qos`,
`vlan`, `timesync`, `tas`, `status`, `wifi`, `fx`, `ota`. The agent replies with an ACK on
`tsn/ack/<id>`. Linux/Raspberry Pi apply QoS/VLAN via `iproute2`+`tc`; ESP32/
STM32/NXP use embedded adapters. The **ESP32 agent** (`esp32-agent/`) is the reference
implementation with zero-touch provisioning.

**ESP32 agent / ESP32-CAM features**:

- **OTA updates** — `{"url":"http://<host>/fw/x.bin"}` on `tsn/cmd/<id>/ota`
  downloads and flashes the image via `esp_https_ota` onto the A/B slot, then reboots;
  a corrupted boot rolls back automatically (see `shared/wtsn_ota`)
- **MQTT last will** — each node registers a retained LWT on `tsn/lwt/<id>`; the GUI
  marks the node offline the moment it disappears
- **Factory reset** — hold the BOOT button (GPIO0) for 3 s on the ESP32 agent: erases
  the provisioning NVS partition and reboots into provisioning mode
- **SNTP time** — the node syncs a NTP clock; sensor / heartbeat payloads carry a
  `ts` timestamp
- **LED status** — solid off = provisioning, blinking = connecting, solid = online
- **Discovery extras** — `tsn/discover` now carries `ip`, `fw` (firmware version) and
  `kind` (`esp32` / `cam`), which the GUI shows per device

---

## TSN node simulator

A **generic TSN Node Simulator** (`tsn-node-simulator`) that simulates arbitrary
TSN-capable controllers (ESP32, Raspberry Pi, STM32, NXP, Linux, or custom kinds)
from plain-text profiles in `profiles/*.ini`.

It simulates discovery, MQTT publishing, QoS, VLAN, time synchronization, TAS / GCL,
sensors and FX over MQTT.

```bash
cmake --build build --target tsn-node-simulator
./build/tsn-node-simulator --all --mqtt-host localhost --mqtt-port 1883
```

See [docs/SIMULATOR.md](docs/SIMULATOR.md).

---

## Project layout

```
src/
  app/        application bootstrap, entry points, tests
  common/     logging, string utils, errors
  mvc/        Model-View-Controller + event bus
  db/         SQLite schema + CRUD repositories
  device/     device model + manager
  discovery/  discovery framework
  qos|vlan|timesync|tas|sensors/   domain services
  stream/     IEEE 802.1Qcc stream reservation (talker/listener)
  mqtt|fxmqtt/        communication integrations
  trace/      communication monitor
  agent/      firmware agent for physical nodes
  simulator/  generic node simulator
  plugin/     loadable protocol plugins (.so)
esp32-agent/   ESP-IDF ESP32 firmware agent
esp32-cam/     ESP-IDF ESP32-CAM firmware (MJPEG stream node)
shared/        shared ESP-IDF components (wtsn_prov provisioning portal, wtsn_version)
microbit-sensor/  micro:bit V2 display panel (wired UART to the ESP agent)
profiles/      device profile templates (.ini)
docs/          ARCHITECTURE, BUILD, SIMULATOR
webgui.py      entry-point shim for the web GUI
wtsn_webgui/   Python web GUI package (MQTT broker, DB, actions, HTTP server)
tests/         Python unit + HTTP smoke tests
launcher/      desktop launcher + autostart
```

See [docs/ARCHITECTURE.md](ARCHITECTURE.md) and [docs/BUILD.md](BUILD.md).

---

## Requirements

| What         | Version    | Purpose                         |
|--------------|-----------|---------------------------------|
| SQLite3      | >= 3.30   | SQLite database                 |
| libmosquitto | >= 2.0 dev| MQTT/FX client                |
| CMake        | >= 3.16   | Build system                   |
| GCC/Clang    | C11        | Compiler                      |
| Python       | >= 3.7    | Web GUI (`webgui.py`)         |
| paho-mqtt    | any       | MQTT client for the web GUI   |

Install on Debian/Ubuntu:

```bash
sudo apt install build-essential cmake libsqlite3-dev libmosquitto-dev python3 python3-pip
python3 -m pip install paho-mqtt
```

If you have no root, install dependencies locally (`$HOME/local`) and point CMake /
pkg-config at them — see [docs/BUILD.md](docs/BUILD.md).

---

## FAQ / notes

- **Simulated sensors** are fixed values bound to the nodes in Devices, so the Sensors
  page is stable instead of continuously drifting.
- **Monitor Pause** keeps buffering new frames so pressing Start resumes where you left off.
- CI (GitHub Actions) builds, tests and packages on every push.

---

## License

MIT
