# Wireless TSN Configurator

> **Central controller for Wireless TSN** — ESP32, Raspberry Pi, STM32, NXP and
> Linux inline with IEEE 802.1Qcc. **Zero-touch node onboarding**: flash the
> agent, connect it to power, and it provisions itself.

A production-grade application for the configuration and control of **Wireless Time Sensitive Networking (Wireless TSN / W-TSN)** nodes such as ESP32, Raspberry Pi, STM32, NXP, Linux and other microcontroller platforms. The **core is written in pure C (C11)** for the control plane, and a **Python web GUI** (`webgui.py`, stdlib only) is the front-end.

It acts as a **central controller (CNC-style control plane, aligned with IEEE 802.1Qcc)** that discovers wireless nodes, manages them, applies QoS / VLAN / time-synchronization / schedule policies, reads sensors, and exposes the whole network over **FXMQTT** — OPC UA FX / C2C Field Exchange carried entirely over MQTT, with a firmware agent for physical devices, a node simulator for virtual ones, and a live communication trace.

---

## Zero-touch onboarding

**Flash it. Power it. It connects itself.**

- On first boot a node with no WiFi credentials starts a **SoftAP `WTSN-Setup`**
  and serves a config portal at **http://192.168.4.1/** — enter your WiFi SSID /
  password and MQTT broker, and it saves them to NVS, reboots, joins your network
  and announces itself to the configurator. No soldering, no serial config.
- New controllers (ESP32, later STM32 / NXP / RPi) use the **same agent
  protocol** — the host `tsn-node-agent` (Linux/RPi) and the embedded agents all
  speak one JSON `/apply` snapshot, so onboarding works the same across hardware.
- WiFi can be changed later remotely via the web GUI (`wifi` command on
  `tsn/cmd/<id>/wifi`).

---

## Setup guide (initialization, step by step)

This walks you through getting a physical **ESP32 agent** online and controllable from
the web GUI. Do these steps once when you set up the system.

### 1. Install dependencies (host)

```bash
sudo apt update
sudo apt install -y build-essential cmake libsqlite3-dev libmosquitto-dev mosquitto python3
```

### 2. Set up the MQTT broker

The broker must listen on all interfaces so the ESP32 (on the same LAN) can reach it:

```bash
echo 'listener 1883 0.0.0.0'        | sudo tee /etc/mosquitto/conf.d/wtsn.conf
echo 'allow_anonymous true'              | sudo tee -a /etc/mosquitto/conf.d/wtsn.conf
sudo systemctl restart mosquitto
```

**Firewall:** the host firewall (UFW on Ubuntu) blocks inbound MQTT by default.
Allow port 1883 (this was the final blocker when the ESP32 could not connect):

```bash
sudo ufw allow 1883/tcp
```

Check the broker is listening on all interfaces:

```bash
ss -tln | grep 1883        # expect LISTEN 0.0.0.0:1883
```

### 3. Get the LAN IP

```bash
ip -4 addr show | grep -oE "inet [0-9.]+" | grep -v 127.0.0.1
# e.g. 192.168.0.149  <-- this is the broker address you will enter on the ESP32
```

### 4. Build & flash the ESP32 agent

```bash
cd esp32-agent
source ~/esp/eim_workspace/v5.3/esp-idf/export.sh          # or your ESP-IDF path
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

If `idf.py -p /dev/ttyUSB0 flash` reports the port is not readable, add yourself
to the `dialout` group and re-login:

```bash
sudo usermod -aG dialout $USER
# log out and back in, then retry the flash
```

On boot the firmware **blinks the onboard LED 3 times** to confirm a successful
flash/reset, then joins your WiFi.

### 5. Provision the WiFi (first boot only)

With no WiFi stored, the agent starts a SoftAP `WTSN-Setup`:

1. Connect your phone/PC to the `WTSN-Setup` SSID (no password).
2. Open **http://192.168.4.1/**.
3. Enter your WiFi SSID, password, and the broker address from step 3
   (e.g. `192.168.0.149`).
4. Save → the agent stores it in NVS, reboots, blinks, joins your WiFi and
   announces itself on MQTT.

### 6. Connect it in the GUI

1. Open the web GUI: **http://127.0.0.1:8000**
2. In the top-right corner switch the mode from **Simulation to REAL**.
3. Set the broker to `192.168.0.149:1883` (FXMQTT / Settings page) and save.
4. Go to **Devices**, add a device with id **`esp32-01`** (kind *ESP32*), Add.
   It appears in the Devices table below.
5. Configure TSN (QoS, VLAN, TimeSync, TAS...) for that device.
6. Click **Execute settings on controller** (bottom bar). The GUI publishes a JSON
   snapshot on `tsn/cmd/esp32-01/apply`; the agent replies on `tsn/ack/esp32-01`.

---

## What it does

- **Automatic device discovery** via MQTT and plugins (extensible for future protocols)
- **Device management** — online / offline / error states, and full info (ID, IP, firmware, last seen, supported TSN features)
- **Centralized configuration (IEEE 802.1Qcc aligned)** — pushes QoS / VLAN / time-sync / schedules to the nodes
- **IEEE 802.1Qcc TSN Stream reservation** — Talker / Listener streams with latency, interval, VLAN and priority; deploy to all nodes over MQTT (`tsn/cmd/<dev>/stream`)
- **IEEE 802.1Q QoS** — priority 0-7, traffic classes, bandwidth reservation, latency requirements
- **IEEE 802.1Q VLAN** — VLAN ID, group membership
- **Time synchronization (gPTP, IEEE 802.1AS)** — grandmaster selection, local / external grandmaster modes
- **Time Aware Scheduling (IEEE 802.1Qbv)** — build and edit Gate Control Lists (GCL), cycle time, deploy schedules, visualize gate windows (full GCL is pushed to the node over `/apply`)
- **Frame Preemption (IEEE 802.1Qbu)** — express frames preempt preemptable classes to protect time-critical traffic
- **Sensor management** — auto-detect temperature, pressure, IMU, distance, GPIO sensors with diagnostics
- **OPC UA FX over MQTT (FXMQTT)** — PubSub / C2C Field Exchange over MQTT
- **MQTT client** — the single communication channel
- **Communication trace / monitoring** in the GUI

All state is persisted in **SQLite**, so devices and configurations survive restarts.
The front-end is a **Python web GUI** (stdlib only) — no extra packages and no
LWGL dependency. Persistence uses SQLite.

---

## IEEE 802.1Qcc TSN Stream reservation

The CNC defines **Talker / Listener streams** (the unit of time-sensitive traffic):

- Each stream binds a **talker** (source) to one or more **listeners** (sinks)
  on a given VLAN, with max latency, max interval, priority and data-frame priority.
- Streams are validated (VLAN 1-4094, priority 0-7, ≥1 listener) and follow
  an 802.1Qcc lifecycle: **configured → ready → (failed/standby)**.
- Deploy (single or all) pushes the stream to every endpoint over MQTT on
  `tsn/cmd/<device>/stream` with a JSON snapshot; both the host agent
  (RPi/Linux) and the ESP32 agent understand it.
- Managed in the web GUI under **IEEE 802.1Qcc → TSN Streams** and in the C
  core via `src/stream/` (`wtsn_tsn_manager`).

---

## Requirements / Dependencies

| What         | Version        | Purpose                        |
|--------------|----------------|--------------------------------|
| SQLite3      | >= 3.30        | SQLite database                |
| libmosquitto | >= 2.0 dev     | MQTT/FX client                 |
| CMake        | >= 3.16        | Build system                  |
| GCC/Clang    | C11 compiler   | Compiler                      |
| Python       | >= 3.7         | Web GUI (`webgui.py`, stdlib) |

Install on Debian/Ubuntu:

```bash
sudo apt install build-essential cmake libsqlite3-dev libmosquitto-dev python3
```

> **GUI:** the front-end is a self-contained Python web GUI (`webgui.py`, stdlib
> only — no extra Python packages). The LVGL C GUI was removed. To expose the web
> GUI over TLS put it behind a reverse proxy (nginx/caddy) — see `webgui.py --help`.

> **Build requirements:** cmake, a C11 compiler (gcc/clang), SQLite3, mosquitto. If you do not have root, you can install all dependencies locally (`$HOME/local`) and point CMake/pkg-config at them — see [docs/BUILD.md](docs/BUILD.md).

---

## How to build & run

```bash
cd wtsn-configurator

# configure and build (C core: CLI + tests + simulator + agent)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)

# run the tests
./build/wtsn-tests

# headless / CLI mode
./build/wtsn-cli --headless --db ./config.db

# web GUI (Python, stdlib only)
python3 webgui.py

# package the binaries
cpack -G TGZ
```

### CLI options

| Flag             | Description                          |
|------------------|--------------------------------------|
| `--db <path>`    | SQLite database file (default `wtsn.db`) |
| `--mqtt-host <h>`| MQTT broker host                    |
| `--mqtt-port <p>`| MQTT broker port (default 1883)    |
| `--plugin-dir <d>`| directory of discovery plugins (.so) |
| `--headless`     | CLI mode, no GUI                    |

---

## OPC UA FX over MQTT (FXMQTT)

The single communication channel. OPC UA FX / C2C Field Exchange is carried
entirely over MQTT through `src/fxmqtt/`:

- **Field Server / Participant** — the configurator (PC) or a selected device
  node acts as the Field Server, configurable from the GUI (FX page).
- **C2C Field Exchange (PubSub topics)** — `tsn/fx/field`, `tsn/fx/data`,
  `tsn/fx/<node>` carry PubSub / C2C traffic on the MQTT broker; no OPC UA
  server, PubSub binary encoding or dedicated multicast stack is required.
- **Broker** — configured via CLI flags / settings (`--mqtt-host`, `--mqtt-port`).

---

## MQTT

- **Client** — `src/mqtt/mqtt_client.c` wraps libmosquitto: connect, subscribe,
  publish, message callbacks, background loop.
- **Broker config** via CLI flags / settings.

---

## Communication Trace / Monitoring

`src/trace/trace.c` (with the **Trace** page in the GUI) records every real and
simulated event live: communication, raw frames, configuration changes and FX multicast
messages — each shown with a timestamp, source and type.

---

## TSN Node Firmware Agent

`tsn-node-agent` runs on physical devices and executes configurator commands over
MQTT:

```bash
cmake --build build --target tsn-node-agent
./build/tsn-node-agent --id node-01 --platform linux --mqtt-host broker.local
```

| Option         | Description                          |
|----------------|--------------------------------------|
| `--id`         | Device id reported to the configurator |
| `--platform`   | `linux`, `raspberry_pi`, `esp32`, `stm32`, `nxp` |
| `--mqtt-host`  | Configurator / broker host          |
| `--mqtt-port`  | MQTT port (default 1883)          |

Commands over `tsn/cmd/<device>`: `apply` (preferred — full JSON snapshot),
`qos`, `vlan`, `timesync`, `tas`, `status`, `wifi` and `fx`. The agent replies
with an ACK on `tsn/ack/<id>`. Linux/Raspberry Pi apply QoS/VLAN via
`iproute2`+`tc`; ESP32/STM32/NXP use the embedded adapters. The **ESP32 agent**
(`esp32-agent/`) is the reference implementation with zero-touch provisioning.

---

## TSN Node Simulator

The repository ships a **generic TSN Node Simulator** (`tsn-node-simulator`). It
does not emulate any specific silicon; it simulates arbitrary TSN-capable controllers
(ESP32, Raspberry Pi, STM32, NXP, Linux, or any custom kind) from plain-text
**configuration profiles** in `profiles/*.ini`.

It simulates: discovery, MQTT publishing, QoS, VLAN, time synchronization, TAS /
Gate Control Lists, sensor simulation (temperature, pressure, IMU, distance, GPIO)
and FX over MQTT.

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
  stream/      IEEE 802.1Qcc TSN stream reservation (talker/listener)
  mqtt|fxmqtt/        communication integrations
  trace/      communication monitor
  agent/      firmware agent for physical nodes
  simulator/  generic node simulator
  plugin/     loadable protocol plugins (.so)
esp32-agent/  ESP-IDF ESP32 firmware agent component
profiles/     device profile templates (.ini)
docs/         ARCHITECTURE, BUILD, SIMULATOR
webgui.py     Python web GUI (stdlib only)
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full design.

---

## Status / Notes

- The core (database, device manager, QoS/VLAN/TimeSync/TAS/sensor logic, MVC,
  plugins, CLI, tests) is implemented.
- The **web GUI (`webgui.py`, Python stdlib only)** shows the whole configurator:
  Dashboard, Devices, QoS (802.1Q), VLAN, TAS (802.1Qbv GCL), Preemption
  (802.1Qbu), TimeSync (gPTP), Sensors, FXMQTT and a live Monitor — it talks
  to the MQTT broker with a bundled stdlib MQTT 3.1.1 client and persists to
  the same sqlite schema.
- The project has been built with GCC on Linux and the full test suite (`wtsn-tests`)
  passes; the CLI, node simulator and node agent all run headless.
- GitHub Actions CI builds, tests and packages on every push.

---

## License

MIT
