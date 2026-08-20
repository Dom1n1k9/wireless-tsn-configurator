# Wireless TSN Configurator - Architecture

## Overview

Pure C, modular, MVC-based desktop application. All persistent state is stored in SQLite.

```
                     ┌────────────────────────────────────────────┐
                     │             GUI (LVGL)                  │
                     │ Dash | Devices | TSN | VLAN | TimeSync   │
                     │ FXMQTT | MQTT | Trace | Settings        │
                     └──────────────────┬─────────────────────────┘
                                        │ events / commands
                     ┌──────────────────▼─────────────────────────┐
                     │             MVC Controller                  │
                     │   routes UI events -> service layer        │
                     │   pushes model notifications -> views      │
                     └──────────────────┬─────────────────────────┘
                                        │
        ┌───────────────────────────────┼───────────────────────────────────┐
        ▼                               ▼               ▼                  ▼
┌───────────────┐  ┌───────────────┐  ┌───────────────┐  ┌───────────────┐
│ Device Manager│  │ QoSMgr VLAN   │  │ TimeSyncMgr   │  │ SensorManager │
│               │  │ TAS/GCL       │  │               │  │               │
└───────┬───────┘  └───────────────┘  └───────────────┘  └───────┬───────┘
        │                                    │                          │
        ▼                                    ▼                          ▼
┌───────────────┐  ┌───────────────┐  ┌───────────────┐  ┌───────────────┐
│ Discoverers   │  │    FXMQTT     │  │  MQTT Client  │  │    Trace     │
│ MQTT/plugins  │  │ FX over MQTT │  │ (mosquitto)   │  │  monitor     │
└───────┬───────┘  └───────┬───────┘  └───────┬───────┘  └───────────────┘
        │                    │ C2C topics      │       ▲
        │                    │ tsn/fx/field    │       │ events
        ▼                    ▼  ─────────▶     ▼       │
┌───────────────┐  ┌───────────────┐  ┌───────────────────┐
│ PluginManager │  │   Agent       │  │     MQTT broker   │
│               │  │ firmware dev  │  │  (mosquitto)      │
└───────┬───────┘  └───────────────┘  └───────────────────┘
        ▼
┌───────────────┐  ┌───────────────┐
│   Simulator   │  │   SQLite DB   │  devices, qos, vlan, tas, timesync, sensors
└───────────────┘  └───────────────┘  virtual nodes
```

## Layers

1. **Common** (`src/common`) - logging, string utilities, error handling.
2. **Model/MVC** (`src/mvc`) - Model base, event bus, controller, view base.
3. **Database** (`src/db`) - SQLite schema init and CRUD repositories.
4. **Services** (`src/device`, `src/qos`, `src/vlan`, `src/timesync`,
   `src/tas`, `src/sensors`) - domain logic.
5. **Discovery** (`src/discovery`) - discover MQTT/plugin devices.
6. **Protocols** (`src/mqtt`, `src/fxmqtt`) - the single MQTT-based
   FX / C2C communication channel.
7. **Agent** (`src/agent`) - firmware agent that executes commands on physical
   nodes (Linux/RPi adapter + ESP32/STM32/NXP embedded adapters).
8. **Simulator** (`src/simulator`) - generic TSN node simulator from profiles.
9. **Trace** (`src/trace`) - communication monitor (comm/frame/config/multicast).
10. **Plugins** (`src/plugin`) - loadable protocol plugins.
11. **UI** (`src/ui`) - LVGL based pages + theme.

## Data Flow

- UI page -> `AppController` -> service -> repository -> SQLite
- Service -> event bus -> UI view updates (device status changes)
- Discoverer -> DeviceManager -> DB (persisted, restored on startup)

## Plugin Architecture

Plugins expose `discover`, `read`, `write`, `probe` functions described by the
`wtsn_plugin_api.h` interface. Discoverers are plugins; the discovery framework
loads them at startup and enumerates discovered nodes.

## FXMQTT Layer (`src/fxmqtt`)

The single communication channel. OPC UA FX / C2C Field Exchange is carried
entirely over MQTT:

- **Field Server / Participant** — the configurator (PC) or a selected device
  node acts as the Field Server, configurable from the GUI FX page.
- **C2C Field Exchange (PubSub topics)** — `tsn/fx/field`, `tsn/fx/data`,
  `tsn/fx/<node>` carry the Field Exchange traffic on the MQTT broker.
- No OPC UA server, PubSub binary encoding or dedicated multicast stack is used.

## Agent Layer (`src/agent`)

`tsn-node-agent` runs on physical nodes and executes configurator commands
(`qos`, `vlan`, `timesync`, `tas`, `status`, `fx`) via MQTT. Linux/RPi
use `iproute2`+`tc`; ESP32/STM32/NXP ship as compile-safe embedded adapters.

## Simulator Layer (`src/simulator`)

Generic TSN node simulator that loads `profiles/*.ini` and simulates each node's
services. See `docs/SIMULATOR.md`.

## Trace Layer (`src/trace` + GUI `trace_page`)

`wtsn_trace` records every communication event, raw frame bytes, configuration
change and FX multicast send (real or simulated) and publishes them on the event
bus. The **Trace** page displays them live: timestamp, source, and kind
(comm/frame/config/multicast).

## Threading Model

- Main thread: GUI event loop.
- Worker threads: discovery pollers, MQTT callbacks, everything else.
- Cross-thread communication via thread-safe event bus (mutex + queue).
