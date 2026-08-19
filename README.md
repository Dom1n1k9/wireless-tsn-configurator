# Wireless TSN Configurator

A production-grade desktop application in pure C for the configuration and control of **Wireless Time Sensitive Networking (Wireless TSN / W-TSN)** nodes such as ESP32, Raspberry Pi, STM32, NXP, Linux and other microcontroller platforms.

It acts as a **central controller (CNC-style control plane, aligned with IEEE 802.1Qcc)** that discovers wireless nodes, manages them, applies QoS / VLAN / time-synchronization / schedule policies, reads sensors, and exposes the whole network over **MQTT** and **OPC UA** — including OPC UA PubSub and OPC UA FX / wireless multicast, with a firmware agent for physical devices, a node simulator for virtual ones, and a live communication trace.

---

## What it does

- **Automatic device discovery** via MQTT, OPC UA and plugins (extensible for future protocols)
- **Device management** — tree view, online / offline / error states, and full info (ID, IP, firmware, last seen, supported TSN features)
- **Centralized configuration (IEEE 802.1Qcc aligned)** — pushes QoS / VLAN / time-sync / schedules to the nodes
- **IEEE 802.1Q QoS** — priority 0-7, traffic classes, bandwidth reservation, latency requirements
- **IEEE 802.1Q VLAN** — VLAN ID, group membership, import / export
- **Time synchronization (gPTP, IEEE 802.1AS)** — grandmaster selection, local / external grandmaster modes
- **Time Aware Scheduling (IEEE 802.1Qbv)** — build and edit Gate Control Lists (GCL), cycle time, deploy schedules, visualize gate windows
- **Frame Preemption (IEEE 802.1Qbu)** — express frames preempt preemptable classes to protect time-critical traffic
- **Sensor management** — auto-detect temperature, pressure, IMU, distance, GPIO sensors with diagnostics
- **OPC UA server + PubSub + FX / wireless multicast**
- **MQTT client** and **MQTT↔OPC UA gateway**
- **Communication trace / monitoring** in the GUI

All state is persisted in **SQLite**, so devices and configurations survive restarts.

---

## Requirements / Dependencies

| What         | Version        | Purpose                        |
|--------------|----------------|--------------------------------|
| SQLite3      | >= 3.30        | SQLite database                |
| libmosquitto | >= 2.0 dev     | MQTT client                   |
| open62541    | >= 1.3 dev     | OPC UA server + PubSub       |
| LVGL         | >= 9.0 dev     | GUI (optional, for GUI mode)  |
| CMake        | >= 3.16        | Build system                  |
| GCC/Clang    | C11 compiler   | Compiler                      |

Install on Debian/Ubuntu:

```bash
sudo apt install build-essential cmake libsqlite3-dev libmosquitto-dev libopen62541-dev
```

> **GUI mode only:** LVGL must be installed as a pkg-config-visible library (e.g. `lvgl`, `liblvgl-dev`). If LVGL is not found, the build falls back to CLI-only automatically.

> **Real OPC UA PubSub:** requires open62541 built with `UA_ENABLE_PUBSUB` and the project built with `-DWTSN_ENABLE_PUBSUB=ON`. Without it, a simulated PubSub backend is used instead.

> **Build requirements:** cmake, a C11 compiler (gcc/clang), SQLite3, mosquitto, open62541 (>= 1.3). If you do not have root, you can install all dependencies locally (`$HOME/local`) and point CMake/pkg-config at them — see [docs/BUILD.md](docs/BUILD.md).

---

## How to build & run

```bash
cd wtsn-configurator

# configure and build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)

# run the tests
./build/wtsn-tests

# headless / CLI mode
./build/wtsn-cli --headless --db ./config.db

# GUI mode (requires LVGL)
./build/wtsn-configurator
```

### CLI options

| Flag             | Description                          |
|------------------|--------------------------------------|
| `--db <path>`    | SQLite database file (default `wtsn.db`) |
| `--mqtt-host <h>`| MQTT broker host                    |
| `--mqtt-port <p>`| MQTT broker port (default 1883)    |
| `--opcua-port <p>`| OPC UA server port (default 4840) |
| `--plugin-dir <d>`| directory of discovery plugins (.so) |
| `--headless`     | CLI mode, no GUI                    |

---

## OPC UA (Server, PubSub, FX)

- **Server** — embedded open62541 server (`src/opcua/opcua_server.c`) hosting a
  configurator namespace, with device node mapping on the Objects tree.
- **PubSub** — dataset-based layer (`src/pubsub/`) with pluggable backends:
  - **Real** — open62541 behind `UA_ENABLE_PUBSUB` (`pubsub_opcua.c`);
  - **Simulated** — loopback backend (`pubsub_loopback.c`) used by the
    simulator and as an automatic fallback.
- **OPC UA FX / wireless multicast** — all W-TSN members (real via the agent,
  or simulated) join a shared multicast group `239.255.0.1:4840` (UA-DP).
  A publisher spreads a dataset to every member over UDP, no broker required.

### FX multicast — simulated
The simulator (`src/simulator/protocol/sim_protocol.c`) models the multi-node
group: every node "joins" `239.255.0.1:4840` and FX sends are logged.

### FX multicast — real
The agent (`src/agent/agent_providers.c`) sends datasets into the group over a
real POSIX UDP multicast socket (`agt_linux_send_fx_multicast`).

---

## MQTT

- **Client** — `src/mqtt/mqtt_client.c` wraps libmosquitto: connect, subscribe,
  publish, message callbacks, background loop.
- **Broker config** via CLI flags / settings.
- **MQTT ↔ OPC UA gateway** — `src/gateway/gateway.c` maps MQTT topics to
  OPC UA paths bidirectionally.
- **MQTT over OPC UA PubSub** — `src/gateway/gateway_pubsub.c` converts MQTT
  topics into PubSub datasets and back.

---

## Communication Trace / Monitoring

`src/trace/trace.c` (with the **Trace** page in the GUI) records every real and
simulated event live: communication, raw frames, configuration changes and FX multicast
messages — each shown with a timestamp, source and type.

---

## TSN Node Firmware Agent

`tsn-node-agent` runs on physical devices and executes configurator commands over
MQTT / OPC UA:

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

Commands over `tsn/cmd/<device>`: `qos`, `vlan`, `timesync`, `tas`, `status`
and `fx` (FX multicast). Linux/Raspberry Pi apply QoS/VLAN via `iproute2`+`tc`;
ESP32/STM32/NXP ship as compile-safe embedded adapters ready for the vendor SDK.

---

## TSN Node Simulator

The repository ships a **generic TSN Node Simulator** (`tsn-node-simulator`). It
does not emulate any specific silicon; it simulates arbitrary TSN-capable controllers
(ESP32, Raspberry Pi, STM32, NXP, Linux, or any custom kind) from plain-text
**configuration profiles** in `profiles/*.ini`.

It simulates: discovery, MQTT publishing, OPC UA endpoints, QoS, VLAN, time
synchronization, TAS / Gate Control Lists, sensor simulation (temperature, pressure,
IMU, distance, GPIO) and OPC UA FX multicast.

```bash
cmake --build build --target tsn-node-simulator
./build/tsn-node-simulator --all --mqtt-host localhost --mqtt-port 1883 --opcua-base 4840
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
  mqtt|opcua|pubsub|gateway/        protocol integrations
  pubsub/     dataset PubSub layer (OPC UA + loopback)
  trace/      communication monitor
  agent/      firmware agent for physical nodes
  simulator/  generic node simulator
  plugin/     loadable protocol plugins (.so)
  ui/         LVGL dark-theme GUI (optional)
profiles/     device profile templates (.ini)
docs/         ARCHITECTURE, BUILD, SIMULATOR
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full design.

---

## Status / Notes

- The core (database, device manager, QoS/VLAN/TimeSync/TAS/sensor logic, MVC,
  plugins, CLI, tests) is implemented.
- The GUI is optional and only built when LVGL is detected. It includes a **Trace**
  page for live communication monitoring.
- **Real OPC UA PubSub is implemented and working** (UADP/ UDP multicast to
  `opc.udp://239.255.0.1:4840/`): the `pubsub_opcua.c` backend creates the full
  writer chain (variables → PublishedDataSet → DataSetFields → WriterGroup →
  DataSetWriter) and publishes dataset snapshots. Build with `-DWTSN_ENABLE_PUBSUB=ON`
  against an open62541 compiled with `UA_ENABLE_PUBSUB`; otherwise the simulated
  (loopback) backend is used automatically as a fallback.
- The project has been built with GCC on Linux and the full test suite (`wtsn-tests`)
  passes; the CLI, node simulator and node agent all run headless.

---

## License

MIT
