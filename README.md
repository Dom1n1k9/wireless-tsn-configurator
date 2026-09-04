# Wireless TSN Configurator

> **Centralized controller for Wireless Time-Sensitive Networking (W-TSN).**
> Manage ESP32 / Raspberry Pi / STM32 / NXP / Linux nodes against **IEEE 802.1Qcc**
> (QoS, VLAN, gPTP/time-sync, TAS/GCL, stream reservation), exposed over **OPC UA FX
> over MQTT (FXMQTT)**. **Zero-touch onboarding**: flash an agent, power it on, and it
> provisions and connects itself.

A production-oriented configuration and control plane for W-TSN. The **control-plane
core is written in pure C (C11)** and ships as a CLI/headless service, a host firmware
agent, and a generic node simulator. The **front-end is a Python web GUI**
(`webgui.py` / `wtsn_webgui/`) with a single-file, dependency-light SPA.

It acts as a centralized controller (CNC-style, aligned with IEEE 802.1Qcc) that
discovers wireless nodes, manages them, applies QoS / VLAN / time-synchronization /
schedule policies, reads sensors, performs firmware OTA and exposes the whole network
over **FXMQTT** — OPC UA FX / C2C Field Exchange carried over MQTT.

---

## Table of contents

1. [Quick start](#quick-start)
2. [Architecture at a glance](#architecture-at-a-glance)
3. [Components](#components)
   - [C core (CLI / headless)](#c-core)
   - [Web GUI](#web-gui)
   - [CONTROLLER / agents](#controllers--agents)
   - [Firmware agents](#firmware-agents)
   - [Simulator](#simulator)
4. [Provisioning & onboarding](#provisioning--onboarding)
5. [MQTT / FXMQTT protocol](#mqtt--fxmqtt-protocol)
6. [Security](#security)
7. [Build & test](#build--test)
8. [Project layout](#project-layout)
9. [Requirements](#requirements)
10. [FAQ / notes](#faq--notes)
11. [License](#license)

---

## Quick start

Everything runs from one machine. See [Provisioning & onboarding](#provisioning--onboarding)
for a physical **ESP32** node.

```bash
# host dependencies (Debian/Ubuntu)
sudo apt update
sudo apt install -y build-essential cmake libsqlite3-dev libmosquitto-dev \
  mosquitto python3 python3-pip
python3 -m pip install paho-mqtt

# build the C core (CLI + tests + simulator + host agent)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)

# run the C tests
./build/wtsn-tests

# launch the web GUI  ->  http://127.0.0.1:8000
python3 webgui.py
```

> **Windows:** the web GUI also runs on Windows and only needs `paho-mqtt`:
> `python -m pip install paho-mqtt`, then `python webgui.py`. The C core targets
> Linux / ESP-IDF.

### Everything with one command — `run.sh`

`run.sh` is the one-launcher script:

```bash
./run.sh                 # MQTT broker + web GUI + browser + provisioning helper
./run.sh --flash         # also build & flash the ESP32 agent first
./run.sh --headless      # services only (broker + GUI + mDNS), no browser/terminal
```

It:
- starts **mosquitto** on `0.0.0.0:1883` (auto-detects your LAN IP),
- launches **`webgui.py`** on http://127.0.0.1:8000 (with a self-healing watchdog,
  log `/tmp/wtsn_mon.log`) and opens the browser,
- advertises this PC as the MQTT broker via **mDNS** (`wtsn-broker.local`) with avahi,
- spawns a terminal showing how to reach the **`WTSN-Setup`** SoftAP
  (http://192.168.4.1/).

**Windows:** `run.ps1` is the equivalent — LAN IP detection, optional mosquitto startup,
GUI health check + restart loop, browser launch:
`.\run.ps1` / `.\run.ps1 -Headless`.

**Desktop launcher** (double-click): run `./launcher/install.sh` once, then double-click
the **"WTSN Configurator"** icon.

**Auto-start at login:** the helper installs a desktop autostart entry that runs
`run.sh --headless`.

---

## Architecture at a glance

```
                     ┌────────────────────────────────────────────┐
                     │        Web GUI (Python webgui.py)         │
                     │  Devices | QoS | VLAN | TAS | Streams |    │
                     │  FXMQTT | Monitor | Sensors | Settings     │
                     └──────────────┬─────────────────────────────┘
                                    │  HTTP / WebSocket / MQTT
                     ┌──────────────▼─────────────────────────────┐
                     │            C11 control core                │
                     │  managers (qos/vlan/timesync/tas/streams)  │
                     │  ─► SQLite  ─► MQTT/FXMQTT  ─► traces      │
                     └──────────────┬─────────────────────────────┘
                                    │  MQTT (mosquitto)
        ┌───────────────────────────┼──────────────────────────────┐
        ▼                           ▼                              ▼
  esp32-agent /              tsn-node-agent                  tsn-node-simulator
  esp32-cam (ESP-IDF)        (host Linux/RPi agent)          (virtual nodes, profiles)
```

- **C core** (`src/`) — modular, dependency-injected managers connected through an
  event bus; all state persisted in **SQLite**; communicates only via **MQTT/FXMQTT**.
- **Web GUI** (`wtsn_webgui/`) — Python (mostly stdlib) HTTP + WebSocket front-end
  with a **Simulation** and a **Real** mode.
- **Firmware agents** (`esp32-agent/`, `esp32-cam/`) — ESP-IDF software for physical
  boards with zero-touch provisioning; a host agent (`tsn-node-agent`) for
  Linux/Raspberry Pi and compile-safe stubs for STM32/NXP.
- **Simulator** (`tsn-node-simulator`) — virtual nodes driven by `profiles/*.ini`.

> **Wireless realism.** True deterministic TSN delivery is not achievable over ordinary
> 802.11, so the project focuses on the *management plane*: QoS, VLAN, TAS/GCL, gPTP and
> stream reservation are configured, applied and monitored over MQTT. The radio layer
> maps 802.1P priorities onto WMM access categories and flags wired-only features
> (e.g. 802.1Qbu preemption) that have no radio meaning. See
> [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Components

### C core

The control-plane engine, built as `wtsn-core` (static lib) with three executables:

| Binary | Purpose |
|--------|---------|
| `wtsn-cli` | headless/CLI controller (`--headless`, `--db`, `--mqtt-host`, `--mqtt-port`, `--plugin-dir`) |
| `tsn-node-agent` | host firmware agent (Linux/RPi adapter) that executes controller commands |
| `tsn-node-simulator` | generic virtual-node simulator from `profiles/*.ini` |

See [docs/BUILD.md](docs/BUILD.md) for platform details.

### Web GUI

A self-contained SPA served on http://127.0.0.1:8000:

| Page       | Purpose |
|------------|---------|
| Devices    | add/remove/ping nodes, status/IP/firmware, **live camera (ESP32-CAM)**, TSN features, **firmware upload + OTA** |
| Domains    | group devices into TSN collision/time domains (organizational) |
| Config Versions | snapshot config, diff + rollback after a deploy |
| Monitor    | live network/frame trace with search filter, **Pause/Start**, Clear |
| Sensors    | live values per node + **1 h history sparklines** |
| FXMQTT     | Field Server / Participant, broker address |
| Sync       | gPTP grandmaster / slave setup |
| QoS / VLAN | 802.1Q priority mapping, VLAN groups + membership |
| TAS / GCL  | gate control lists with **visual gate windows** |
| Preemption | eMAC / pMAC priority split (802.1Qbu) |
| Streams    | 802.1Qcc talker / listener reservations + deploy |
| Settings   | broker, DB info, **Export / Restore configuration (JSON)** |

> **Domains today:** the Domains page is **organizational** — it stores the
> per-cell grouping and the device → domain assignment, but *Execute settings*
> still deploys globally and does not scope QoS/VLAN/TAS to a domain yet.

Run it directly:

```bash
python3 webgui.py [--host H] [--port P]
# env: WTSN_HOST, WTSN_PORT, WTSN_DB, WTSN_BROKER, WTSN_USER, WTSN_PASS,
#      WTSN_WEB_USER, WTSN_WEB_PASS, WTSN_TLS_CA, WTSN_TLS_CERT, WTSN_TLS_KEY
```

**Modes.** *Simulation* fabricates a stable set of nodes, sensors and a frame flow so
everything can be exercised without hardware. *Real* connects to your MQTT broker and
live devices (commands are only published in Real mode).

> **TLS in the GUI.** TLS itself is not bundled — put the GUI behind a reverse proxy
> (nginx/caddy) for HTTPS; MQTT TLS is optional via the `WTSN_TLS_*` env vars above.

### Controllers / agents

Controllers (this CNC, or future STM32/NXP/RPi endpoints) and embedded agents share one
**agent protocol** — a single JSON `/apply` snapshot on `tsn/cmd/<id>/apply`.
The host `tsn-node-agent` runs on a PC/RPi and applies QoS/VLAN via `iproute2`+`tc`
(see [docs/BUILD.md](docs/BUILD.md)).

### Firmware agents

- **`esp32-agent/`** — the reference ESP32 (ESP-IDF v5.x) agent:
  - **zero-touch provisioning** — SoftAP `WTSN-Setup-<id>` + portal
    (http://192.168.4.1/), NVS storage, auto re-provision as fallback,
  - **MQTT command execution** — `apply` (JSON snapshot), `qos`, `vlan`, `timesync`,
    `tas`, `stream`, `preemption`, `status`, `wifi`, `fx`, `actor`, `ping`, `identify`,
    `ota`, `reset`, `reboot`, `factory`,
  - **TSN config persisted to NVS** and restored on reboot,
  - **software gPTP** over UDP multicast (`224.0.1.129`, best-effort, sub-ms on WiFi),
  - **sensors** — BME280 (bit-bang I2C), TEMT6000 light, HC-S501 PIR; 1 s heartbeat +
    1 h history + sparklines in the GUI,
  - **relay actor** — GPIO16 relay pulse on motion (auto-detected sensor/relay role),
  - **OTA** — A/B slots with automatic rollback on bad boot (`shared/wtsn_ota`),
  - **LWT last-will**, **SNTP** time, **LED** status (provisioning = fast blink,
    connecting = blink, online = solid), **factory reset** (BOOT 3 s),
  - **UART panel** — micro:bit V2 display over wired UART with a **CRC-16/CCITT**
    checksum on every frame (`T/P/H/L/M/A` values).
- **`esp32-cam/`** — ESP32-CAM node streaming MJPEG to the Devices page, provisioned
  with the same shared portal, OTA-capable.
- **micro:bit sensor panel** (`microbit-sensor/`) — MakeCode (`main.ts`) or MicroPython
  (`microbit_sensor.py`) variant; B/A button cycling, beep on PIR motion.

See [esp32-agent/README.md](esp32-agent/README.md) for the full firmware protocol and
wiring tables, and `docs/SIMULATOR.md` for the virtual nodes.

---

## Provisioning & onboarding

**Flash it. Power it. It connects itself.**

- On first boot (no WiFi credentials) a node starts a **SoftAP `WTSN-Setup-<id>`**
  serving the config portal at **http://192.168.4.1/** — enter WiFi SSID / password and
  the MQTT broker; the agent saves them to NVS, reboots, joins your network and
  announces itself on MQTT.
- Each board advertises a **unique SSID** (`WTSN-Setup-<device-id>`) so multiple boards
  are distinguishable in setup mode.
- If a node loses its network it gives up after a few failed reconnects and
  **automatically restarts the SoftAP + portal** for over-the-air re-provisioning.
- WiFi can be changed later from the web GUI (`wifi` command on `tsn/cmd/<id>/wifi`).

> **Client isolation:** from a phone/mac hotspot, client isolation can block the node.
> Prefer a normal router WiFi.

For the step-by-step setup (broker, provisioning, connecting in the GUI) see
[esp32-agent/README.md](esp32-agent/README.md#wiring-to-the-web-gui-real-mode).

---

## MQTT / FXMQTT protocol

MQTT is the **single communication channel**. The controller publishes commands to
`tsn/cmd/<id>/<command>` and subscribes to the status/discovery/ack/LWT/sensor feeds.

| Topic (pattern)        | Direction | Purpose |
|------------------------|-----------|---------|
| `tsn/cmd/<id>/apply`   | out | full JSON snapshot (preferred) |
| `tsn/cmd/<id>/{qos,vlan,timesync,tas,stream,preemption,status,wifi,fx,ping,identify,ota,reset,actor,reboot,factory}` | out | per-feature commands |
| `tsn/ack/<id>`         | in  | reply `{"id","ok"[, "ip"]}` |
| `tsn/status`           | in  | heartbeat / status JSON (rssi, fw, ip) |
| `tsn/discover`         | in  | on-connect announcement (`ip`, `fw`, `kind`) |
| `tsn/lwt/<id>`         | in  | retained last-will → node marked offline |
| `tsn/sensors`, `tsn/sensors/<id>/...`, `tsn/sensors/event` | in | telemetry / history |
| `tsn/ptp`              | in  | gPTP reports |
| `tsn/fx/cmd/<id>`      | out | FX / C2C field-exchange commands |
| `tsn/fx/data`, `tsn/fx/<id>` | in/out | FX data feed (motion events etc.) |
| `tsn/cmd/<id>/stream` + `tsn/fx/cmd/<id>` | out | 802.1Qcc stream reservation |

> **Terminology:** the C plugin uses `tsn/discovery` (a legacy alias); everything else
> uses `tsn/discover`.

---

## Security

### Provisioning (plaintext by default)

The provisioning portal is intentional **plain HTTP on an open SoftAP** (no TLS, no AP
password): the WiFi password is transmitted in cleartext from your phone/PC to the
board. This is the standard zero-touch trade-off, but it means:

- anyone on the `WTSN-Setup-<id>` SoftAP can read the credentials being entered;
- the portal is only reachable from that SoftAP, so the exposure window is the few
  minutes you spend provisioning.

For sensitive deployments, either keep provisioning physically supervised or:

- **Lock the SoftAP behind WPA2-PSK:** store a password in NVS (namespace `wtsn`, key
  `ap_pass`, min 8 chars); the compile-time fallback is `PROV_AP_PASS_DEFAULT` in
  `shared/wtsn_prov/wtsn_prov.c`. After provisioning the SoftAP is gone and normal
  operation only uses MQTT.
- **Secure the MQTT channel:** broker username/password + TLS are supported on both the
  agent and the GUI (see below).

> **WiFi password hygiene:** the node **never reports its saved WiFi password back** and
> `tsn/cmd/<id>/wifi` is accepted with an *optional* `pass` field — when the password is
> omitted, the agent keeps whatever is stored in NVS. So re-pointing an already-provisioned
> node to a new SSID does not re-send the secret over plaintext MQTT.

### MQTT authentication & TLS

Two independent configuration paths — the **agent** (NVS) and the **web GUI** (env):

| Component | Auth / TLS configuration |
|-----------|--------------------------|
| ESP32 agent / CAM | NVS keys in namespace `wtsn`: `muser`, `mpass`, `mtls` (1=on), `mtls_ca` (PEM), `minsec` (1 = skip verify, dev only) |
| Web GUI | env: `WTSN_USER`, `WTSN_PASS`, `WTSN_TLS_CA`, `WTSN_TLS_CERT`, `WTSN_TLS_KEY`, `WTSN_TLS_INSECURE=1` (dev) |
| Broker | run mosquitto with authentication and/or TLS listeners on the machine |

### Web GUI access

The web GUI supports optional HTTP Basic auth via `WTSN_WEB_USER`/`WTSN_WEB_PASS`
(compared in constant time). There is no built-in HTTPS — terminate TLS in a reverse
proxy (nginx/caddy).

---

## Build & test

```bash
# configure + build (C core: CLI, tests, agent, simulator)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)

# run C tests (includes the host JSON parser test via ctest)
./build/wtsn-tests
(cd build && ctest --output-on-failure)

# headless controller
./build/wtsn-cli --headless --db ./config.db

# host firmware agent
./build/tsn-node-agent --id node-01 --platform linux --mqtt-host broker.local

# generic simulator (all profiles)
./build/tsn-node-simulator --all --mqtt-host localhost --mqtt-port 1883

# Python tests (web GUI) + lint
python3 -m unittest discover -s tests
python3 -m ruff check webgui.py wtsn_webgui tests

# package
cpack -G TGZ          # or: cmake --build build --target package
```

**Build options:** `-DBUILD_GUI=ON|OFF` (install the Python web GUI, default ON),
`-DBUILD_PLUGINS=ON|OFF` (build the sample MQTT discovery plugin, default ON).

**CI** (GitHub Actions) runs, on every push/PR:
- host build + `wtsn-tests` + packaging,
- Python lint (ruff) + `unittest`,
- **AddressSanitizer/UBSan** build + tests (blocking),
- **cppcheck** static analysis (blocking),
- ESP-IDF build of both firmwares (`esp32-agent`, `esp32-cam`).

See [docs/BUILD.md](docs/BUILD.md) for no-root (local install) and packaging details,
and [docs/SIMULATOR.md](docs/SIMULATOR.md) for the simulator profiles and options.

---

## Project layout

```
src/                  C11 control-plane core
  app/                application bootstrap, entry points, tests
  common/             logging, string utils, errors
  mvc/                model + event bus (GUI controller/view removed)
  db/                 SQLite schema + CRUD repositories
  device/             device model + manager
  discovery/          discovery framework
  qos|vlan|timesync|tas|sensors/   domain services
  stream/             IEEE 802.1Qcc stream reservation (talker/listener)
  mqtt|fxmqtt/        MQTT + OPC UA FX over MQTT
  radio/              WMM/802.11e mapping (802.1P -> AC)
  domain/             per-cell TSN domains
  config_version/     config snapshots + diff/rollback
  telemetry|trace/    telemetry + live communication monitor
  agent/              host firmware agent (Linux/RPi adapter)
  simulator/          generic node simulator
  plugin/             loadable protocol plugins (.so)
esp32-agent/          ESP-IDF ESP32 firmware agent (reference)
esp32-cam/            ESP-IDF ESP32-CAM firmware (MJPEG stream node)
shared/               shared ESP-IDF components (wtsn_prov, wtsn_ota, wtsn_version)
microbit-sensor/      micro:bit V2 display panel (wired UART, MakeCode + MicroPython)
profiles/             device profile templates (.ini) for the simulator
docs/                 ARCHITECTURE, BUILD, SIMULATOR
webgui.py             entry-point shim for the web GUI
wtsn_webgui/          Python web GUI package
tests/                Python unit + HTTP smoke tests
launcher/             desktop launcher + autostart
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/BUILD.md](docs/BUILD.md).

---

## Requirements

| What          | Version   | Purpose                        |
|---------------|-----------|--------------------------------|
| SQLite3       | >= 3.30   | SQLite database                |
| libmosquitto  | >= 2.0 dev| MQTT/FX client                 |
| CMake         | >= 3.16   | Build system                   |
| GCC/Clang     | C11       | Compiler                       |
| Python        | >= 3.7    | Web GUI (`webgui.py`)          |
| paho-mqtt     | any       | MQTT client for the web GUI    |

Install on Debian/Ubuntu:

```bash
sudo apt install build-essential cmake libsqlite3-dev libmosquitto-dev \
  python3 python3-pip
python3 -m pip install paho-mqtt
```

If you have no root, build the dependencies locally (`$HOME/local`) and point
CMake/pkg-config at them — see [docs/BUILD.md](docs/BUILD.md).

---

## FAQ / notes

- **Simulated sensors** are fixed values bound to the nodes in Devices, so the Sensors
  page is stable instead of continuously drifting.
- **Monitor Pause** keeps buffering new frames so pressing Start resumes where you left
  off.
- **The GUI restarts itself** — a watchdog re-checks the web GUI health every 4 s and
  restarts it if it wedges (`/tmp/wtsn_mon.log`).
- **UART integrity** — the ESP32 ↔ micro:bit link appends a CRC-16/CCITT checksum to
  every line; corrupted frames are dropped instead of showing wrong values.
- **CI** (GitHub Actions) builds, tests (incl. ASan/UBSan + cppcheck) and packages on
  every push.
- **`tsn/discovery` vs `tsn/discover`** — a few legacy topics still use the old name;
  both are understood.

---

## License

MIT
