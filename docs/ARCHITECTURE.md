# Wireless TSN Configurator - Architecture

## Overview

Pure C, modular, MVC-based desktop application. All persistent state is stored in SQLite.

> **Scope note (wireless realism):** True deterministic TSN delivery is not
> achievable over ordinary 802.11. This project therefore focuses on the
> *management plane*: configuring QoS, VLAN, TAS, gPTP and streams on wireless
> nodes and monitoring them over MQTT. The `wtsn_radio` layer maps those wired
> TSN concepts onto the WMM/802.11e radio queues and flags features (e.g.
> 802.1Qbu preemption) that have no radio meaning.

```
                     ┌────────────────────────────────────────────┐
                     │        Web GUI (Python webgui.py)         │
                     │ Dash | Devices | TSN | VLAN | TimeSync    │
                     │ FXMQTT | Monitor | Sensors | Settings     │
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
└───────┬───────┘  └───────┬───────┘  └───────┬───────┘  └───────┬───────┘
        │                  │                  │                  │
        ▼                  ▼                  ▼                  ▼
┌───────────────┐  ┌───────────────┐  ┌───────────────┐  ┌───────────────┐
│ Discoverers   │  │    FXMQTT     │  │  MQTT Client  │  │    Trace     │
│ MQTT/plugins  │  │ FX over MQTT │  │ (mosquitto)   │  │  monitor     │
└───────┬───────┘  └───────┬───────┘  └───────┬───────┘  └───────────────┘
        │                  │ C2C topics      │       ▲
        │                  │ tsn/fx/field    │       │ events
        ▼                  ▼  ─────────▶     ▼       │
┌───────────────┐  ┌───────────────┐  ┌───────────────────┐
│ PluginManager │  │   Agent       │  │     MQTT broker   │
│               │  │ firmware dev  │  │  (mosquitto)      │
└───────┬───────┘  └───────────────┘  └───────────────────┘
        ▼
┌───────────────┐
│   Simulator   │
│               │
└───────────────┘
                 ┌───────────────┐  ┌───────────────┐  ┌───────────────┐
                 │  Radio layer  │  │  Domain Mgmt  │  │ Config Vers.  │
                 │  (WMM/802.11e)│  │ (TSN domains) │  │ diff/rollback │
                 └───────────────┘  └───────────────┘  └───────────────┘
                 ┌───────────────┐  ┌───────────────┐  ┌───────────────┐
                 │ Trace (SQLite)│  │ gPTP reports  │  │ Heartbeat FSM │
                 │  persisted    │  │ over-the-air  │  │ online/offline │
                 └───────────────┘  └───────────────┘  └───────────────┘
```

## Layers

1. **Common** (`src/common`) - logging, string utilities, error handling.
2. **Model/MVC** (`src/mvc`) - Model base, event bus, controller, view base.
3. **Database** (`src/db`) - SQLite schema init and CRUD repositories.
4. **Services** (`src/device`, `src/qos`, `src/vlan`, `src/timesync`,
   `src/tas`, `src/sensors`, `src/radio`, `src/domain`,
   `src/config_version`) - domain logic.
5. **Discovery** (`src/discovery`) - discover MQTT/plugin devices.
6. **Protocols** (`src/mqtt`, `src/fxmqtt`) - the single MQTT-based
   FX / C2C communication channel.
7. **Agent** (`src/agent`) - firmware agent that executes commands on physical
   nodes (Linux/RPi adapter + ESP32/STM32/NXP embedded adapters).
8. **Simulator** (`src/simulator`) - generic TSN node simulator from profiles.
9. **Trace** (`src/trace`) - communication monitor (comm/frame/config/multicast),
   now persisted to SQLite (`trace_log` table).
10. **Plugins** (`src/plugin`) - loadable protocol plugins.
11. **Web GUI** (`webgui.py`) - Python stdlib-only web front-end that
    publishes/consumes the same MQTT topics and persists to the same sqlite
    schema.

## Radio Layer (`src/radio`)

Maps 802.1P priorities onto WMM access categories (AC_VO/AC_VI/AC_BE/AC_BK)
per 802.11-2016 Table 9-2. Used by the stream/CNN path to derive which radio
queue a stream lands on, and flags wired-only TSN features (e.g. 802.1Qbu
preemption) that have no meaning inside a single radio link.

## TSN Domains (`src/domain`)

Physical 802.11 cells each form their own collision/time domain. Devices are
assigned to a domain; QoS/VLAN/TAS configurations can then be scoped per
domain rather than treating the whole fleet as a single domain.

## Config Versioning (`src/config_version`)

Snapshots the configuration scope (devices + QoS + VLAN) as canonical strings
so operators can diff two versions and roll back after a failed deploy.

## Data Flow

- UI page -> `AppController` -> service -> repository -> SQLite
- Service -> event bus -> UI view updates (device status changes)
- Discoverer -> DeviceManager -> DB (persisted, restored on startup)
- Heartbeat -> DeviceManager -> online/offline state machine -> DB + event bus
- OTA sync report -> TimesyncManager -> `timesync_reports` DB + event bus

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

C core: Main thread runs the headless ops loop. Worker threads: discovery
pollers, MQTT callbacks. Cross-thread communication via event bus.

Web GUI: `ThreadingHTTPServer` spawns one thread per request (daemon).
A background `sim_runner` thread and an `mqtt_listener_loop` thread run
independently; sqlite connections are opened per-request/thread.
