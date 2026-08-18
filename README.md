# Wireless TSN Configurator

A production-grade desktop application in pure C for the configuration and control of **Wireless Time Sensitive Networking (Wireless TSN / W-TSN)** nodes such as ESP32, Raspberry Pi, STM32 and future microcontroller platforms.

It acts as a central controller that discovers wireless nodes, manages them, applies QoS / VLAN / time-synchronization / schedule policies, reads sensors, and exposes the whole network over **MQTT** and **OPC UA**.

---

## What it does

- **Automatic device discovery** via MQTT and OPC UA (plugin-based for future protocols)
- **Device management** with a tree view, online/offline/error states and full info (ID, IP, firmware, last seen, supported TSN features)
- **IEEE 802.1Q QoS configuration** (priority 0-7, traffic classes, bandwidth reservation, latency requirements)
- **VLAN-like wireless group management** (VLAN ID, membership, import/export)
- **Time synchronization** with grandmaster selection, local / external grandmaster modes and future gPTP / 802.1AS support
- **Time Aware Scheduling (TAS)** — build and edit Gate Control Lists (GCL), configure cycle time, deploy schedules to nodes and visualize gate open/close windows
- **Sensor management** — auto-detect temperature, pressure, IMU, distance and GPIO sensors with diagnostics
- **Integrated OPC UA server** (open62541) with node browser and data model mapping
- **Integrated MQTT client** (mosquitto) with topic browser and pub/sub
- **MQTT over OPC UA gateway** with bidirectional translation

Everything is persisted in **SQLite**, so devices and their configurations are remembered across restarts.

---

## Requirements / Dependencies

| What         | Version        | Purpose                        |
|--------------|----------------|--------------------------------|
| SQLite3      | >= 3.30        | SQLite database                |
| libmosquitto | >= 2.0 dev     | MQTT client                   |
| open62541    | >= 1.3 dev     | OPC UA server                 |
| LVGL         | >= 9.0 dev     | GUI (optional, for GUI mode)  |
| CMake        | >= 3.16        | Build system                  |
| GCC/Clang    | C11 compiler   | Compiler                      |

Install on Debian/Ubuntu:

```bash
sudo apt install build-essential cmake libsqlite3-dev libmosquitto-dev libopen62541-dev
```

> **GUI mode only:** LVGL also needs to be installed as a pkg-config-visible library (e.g. `lvgl`, `liblvgl-dev`). If LVGL is not found, the build falls back to CLI-only automatically.

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

## TSN Node Simulator

The repository also ships a **generic TSN Node Simulator** (`tsn-node-simulator`). It does not emulate any specific silicon; instead it simulates arbitrary TSN-capable controllers (ESP32, Raspberry Pi, STM32, NXP, Linux, or any custom kind) driven by plain-text **configuration profiles** in `profiles/*.ini`.

The simulator exposes: device discovery, MQTT publishing, OPC UA endpoints, QoS, VLAN, time synchronization, TAS and Gate Control Lists, and sensor simulation (temperature, pressure, IMU, distance, GPIO).

```bash
cmake --build build --target tsn-node-simulator
./build/tsn-node-simulator --all --mqtt-host localhost --mqtt-port 1883 --opcua-base 4840
```

Create your own node by adding a profile:

```ini
[device]
id = my-node
name = Custom Node
kind = nxp
model = iMX8M
...
```

See [docs/SIMULATOR.md](docs/SIMULATOR.md).

---

## Project layout

```
src/
  simulator/     # generic TSN node simulator (independent feature)
profiles/        # device profile templates (.ini)
  ...
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full design.

---

## Status / Notes

- The core (database, device manager, QoS/VLAN/TimeSync/TAS/sensor logic, MVC, plugins, CLI, tests) is implemented.
- The GUI is optional and only built when LVGL is detected.
- The OPC UA server uses the open62541 API and may need minor version-specific adjustments — run `./build/wtsn-tests` first after cloning.
- This project has not yet been through a full CI build on a Linux machine; it is the initial scaffold.
