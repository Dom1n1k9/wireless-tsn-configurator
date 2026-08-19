# Wireless TSN Configurator - Architecture

## Overview

Pure C, modular, MVC-based desktop application. All persistent state is stored in SQLite.

```
                     ┌────────────────────────────────────────────┐
                     │             GUI (LVGL)                  │
                     │ Dash | Devices | TSN | VLAN | TimeSync   │
                     │ OPC UA | MQTT | Trace | Settings       │
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
│ Discoverers   │  │    PubSub     │  │  MQTT Client  │  │    Trace     │
│ MQTT/OPC UA  │  │ OPC UA+Loop  │  │ (mosquitto)   │  │  monitor     │
└───────┬───────┘  └───────┬───────┘  └───────┬───────┘  └───────────────┘
        │                    │     OPC UA FX        │       ▲
        │                    │  multicast 239.255.0.1│       │ events
        ▼                    ▼  ─────────▶          ▼       │
┌───────────────┐  ┌───────────────┐  ┌───────────────────┐
│ OPC UA Server │  │   Agent       │  │ Gateway (MQTT↔UA   │
│ (open62541)   │  │ firmware dev  │  │  + MQTT-over-PubSub)│
└───────┬───────┘  └───────────────┘  └───────────────────┘
        ▼
┌───────────────┐  ┌───────────────┐
│ PluginManager │  │   Simulator   │  virtual nodes
└───────┬───────┘  └─────────────────┘
        ▼
┌───────────────┐
│   SQLite DB   │  devices, qos, vlan, tas, timesync, sensors
└───────────────┘
```

## Layers

1. **Common** (`src/common`) - logging, string utilities, error handling.
2. **Model/MVC** (`src/mvc`) - Model base, event bus, controller, view base.
3. **Database** (`src/db`) - SQLite schema init and CRUD repositories.
4. **Services** (`src/device`, `src/qos`, `src/vlan`, `src/timesync`,
   `src/tas`, `src/sensors`) - domain logic.
5. **Discovery** (`src/discovery`) - discover MQTT/OPC UA devices.
6. **Protocols** (`src/mqtt`, `src/opcua`, `src/pubsub`, `src/gateway`) -
   MQTT, OPC UA server/PubSub/FX and gateways.
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

## PubSub Layer (`src/pubsub`)

A uniform dataset-based PubSub abstraction with pluggable backends:

- **OPC UA PubSub** (`pubsub_opcua.c`) - real open62541 PubSub behind
  `UA_ENABLE_PUBSUB`; used by the configurator against real devices.
- **Loopback** (`pubsub_loopback.c`) - simulated PubSub, used by the simulator
  and as a fallback so the gateway keeps working without a fully-built PubSub stack.

The **MQTT-over-OPC-UA-PubSub gateway** (`gateway_pubsub.c`) bidirectionally
converts MQTT topics into PubSub datasets and back.

## FX Wireless Multicast

All W-TSN members share a multicast group `239.255.0.1:4840` (UA-DP
transport), so a publisher reaches every subscriber directly over UDP — no MQTT
broker needed. Two providers exist:

- **Real** — the agent (`src/agent/agent_providers.c`) sends datasets into the
  group over a POSIX UDP multicast socket (`agt_linux_send_fx_multicast`).
- **Simulated** — the simulator (`src/simulator/protocol/sim_protocol.c`)
  models the multi-node group and logs each FX send.

## Agent Layer (`src/agent`)

`tsn-node-agent` runs on physical nodes and executes configurator commands
(`qos`, `vlan`, `timesync`, `tas`, `status`, `fx`) via MQTT/OPC UA. Linux/RPi
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
- Worker threads: discovery pollers, MQTT callbacks, OPC UA server, gateway.
- Cross-thread communication via thread-safe event bus (mutex + queue).
