# Wireless TSN Configurator — Architecture

## Overview

The project is a two-part system:

1. **C11 control-plane core** (`src/`) — a modular, manager-based engine with all
   persistent state in **SQLite** and a single communication channel over **MQTT /
   FXMQTT**.
2. **Python web GUI** (`wtsn_webgui/`) — a stdlib-light HTTP + WebSocket front-end that
   speaks the same MQTT topics and persists to the same SQLite schema (one DB for
   *simulation*, one for *real* mode).

> **Scope note (wireless realism):** True deterministic TSN delivery is not achievable
> over ordinary 802.11. This project therefore focuses on the *management plane*:
> configuring QoS, VLAN, TAS, gPTP and streams on wireless nodes and monitoring them
> over MQTT. The `wtsn_radio` layer maps those wired TSN concepts onto WMM/802.11e radio
> queues and flags features (e.g. 802.1Qbu preemption) that have no radio meaning.

```
                     ┌────────────────────────────────────────────┐
                     │        Web GUI (Python wtsn_webgui)        │
                     │  Devices | QoS | VLAN | TAS | Streams |     │
                     │  FXMQTT | Monitor | Sensors | Settings      │
                     │  sim    real  (MQTT link)  actions/ HTTP    │
                     └──────────────────┬──────────────────────────┘
                                        │  HTTP/WS + MQTT topics
                     ┌──────────────────▼──────────────────────────┐
                     │        C11 control-plane core (src/)        │
                     │  app (composition root)                     │
                     │  └─ managers: device, qos, vlan, timesync,  │
                     │     tas, stream, sensors, domain,           │
                     │     config_version, radio                   │
                     │  └─ mvc: model + event bus                  │
                     │  └─ db: SQLite schema + repositories        │
                     └──────────────────┬──────────────────────────┘
                                        │  MQTT (mosquitto)
        ┌───────────────────────────────┼───────────────────────────────────┐
        ▼                               ▼                                   ▼
  tsn-node-agent                  tsn-node-simulator                eSP32 agent / CAM
  host (Linux/RPi)                virtual nodes                     (ESP-IDF firmware)
  iproute2 + tc                   profiles/*.ini                    zero-touch provisioning
```

## Layers (C core)

1. **Common** (`src/common`) — logging, string utilities, error handling (`wtsn_strlcpy`,
   bounds-safe copying used throughout).
2. **Model / MVC** (`src/mvc`) — `wtsn_model` + `wtsn_event_bus`. The original GUI
   controller/view skeleton was removed with the old GUI; the model/event-bus part is
   what managers use to notify the UI and dispatch events.
3. **Database** (`src/db`) — SQLite schema (16 tables), CRUD repositories, **versioned
   migrations** via `PRAGMA user_version`, **`PRAGMA foreign_keys=ON`** with `ON DELETE
   CASCADE` on the FK relationships (deleting a device cleans up its QoS/VLAN/sensors,
   deleting a stream its members, etc.).
4. **Services** (`src/device`, `src/qos`, `src/vlan`, `src/timesync`, `src/tas`,
   `src/sensors`, `src/radio`, `src/domain`, `src/config_version`, `src/stream`) —
   domain logic; each manager is constructed with `(db, event_bus)` (dependency
   injection via the composition root in `src/app/app.c`).
5. **Discovery** (`src/discovery`) — discover MQTT/plugin devices; the MQTT discovery
   plugin subscribes to the announce topic and feeds the device manager.
6. **Protocols** (`src/mqtt`, `src/fxmqtt`) — the single MQTT-based FX / C2C
   communication channel (see below).
7. **Agent** (`src/agent`) — host firmware agent executing controller commands on a
   physical node (Linux/RPi adapter via `iproute2` + `tc`; ESP32/STM32/NXP embedded
   adapters ship as compile-safe stubs).
8. **Simulator** (`src/simulator`) — generic TSN node simulator from `profiles/*.ini`.
9. **Trace / telemetry** (`src/trace`, `src/telemetry`) — live communication monitor
   persisted to SQLite (`trace_log`), plus telemetry helpers.
10. **Plugins** (`src/plugin`) — dlopen-able protocol plugins (e.g. MQTT discovery).

## Radio Layer (`src/radio`)

Maps 802.1P priorities onto WMM access categories (AC_VO / AC_VI / AC_BE / AC_BK) per
802.11-2016 Table 9-2. Used by the stream/CNC path to derive which radio queue a stream
lands on, and flags wired-only TSN features (e.g. 802.1Qbu preemption) that have no
meaning inside a single radio link.

## TSN Domains (`src/domain`)

Physical 802.11 cells each form their own collision/time domain. Devices are assigned to
a domain; QoS/VLAN/TAS configurations *could* be scoped per domain rather than treating
the whole fleet as a single domain.

> **Current scope:** domains are **organizational** — the assignment is stored in
> SQLite and managed from the web GUI's *Domains* page, but the deploy path
> (`exec_all` / `/apply`) is still global and does not yet filter by domain. The
> plumbing (`db_domains`, `domain_manager`, `devices.domain`) is in place for a future
> per-domain-scoped deploy.

## Config Versioning (`src/config_version`)

Snapshots the configuration scope (devices + QoS + VLAN + streams + TAS …) as canonical
strings so operators can diff two versions and roll back after a failed deploy. The DB
stores each snapshot's payload (up to 64 KB) in `config_versions`.

## Data Flow

- UI page → action handler → repository → SQLite
- Manager → event bus → UI notification (device status changes)
- Discoverer → DeviceManager → DB (persisted, restored on startup)
- Heartbeat → DeviceManager → online/offline state machine → DB + event bus
- Sync report → TimesyncManager → `timesync_reports` DB + event bus

## Plugin Architecture

Plugins expose `discover`, `read`, `write`, `probe` functions described by the
`wtsn_plugin_api.h` interface. Discoverers are plugins; the discovery framework loads
them at startup and enumerates discovered nodes.

## FXMQTT Layer (`src/fxmqtt` + `wtsn_webgui`)

The single communication channel. OPC UA FX / C2C Field Exchange is carried entirely
over MQTT:

- **Field Server / Participant** — the configurator (PC) or a selected device node acts
  as the Field Server, configurable from the GUI FX page.
- **C2C Field Exchange topics** — `tsn/fx/cmd/<id>` (commands, e.g. stream reservation),
  `tsn/fx/data` (shared data feed such as PIR motion events), `tsn/fx/<node>` (per-node
  field exchange).
- No OPC UA server, PubSub binary encoding or dedicated multicast stack is used.

## Agent Layer (`src/agent`)

`tsn-node-agent` runs on physical nodes and executes configurator commands (`qos`,
`vlan`, `timesync`, `tas`, `status`, `fx`) via MQTT. Linux/RPi use `iproute2` + `tc`;
ESP32/STM32/NXP ship as compile-safe embedded adapters. The ESP32 reference
implementation lives in `esp32-agent/` (see its README).

## Web GUI (`wtsn_webgui`)

Decomposed package (originally a single 1,700-line file) with clear separation:

| Module | Responsibility |
|--------|----------------|
| `state.py` | shared mutable state + locks (events, acks, mode, MQTT client lock) |
| `db.py` | SQLite schema, migrations, event trace, loaders, history |
| `mqtt_broker.py` | paho wrapper: synchronous, thread-safe broker surface (with optional TLS via `WTSN_TLS_*`) |
| `mqtt_link.py` | real-mode broker cache + background listener loop (status/ack/discover/LWT/sensors) |
| `sim.py` | simulation engine (stable virtual devices, sensors, frames) |
| `actions/` | per-domain action handlers (devices, qos, vlan, tas, timesync, streams, domain, fxmqtt, misc) behind a thin dispatcher |
| `server.py` | HTTP server, JSON API, hand-rolled WebSocket, basic auth, firmware serving |
| `static/index.html` | single-file SPA (plain JS, no framework/build step) |

**Threading model.** The web GUI uses `ThreadingHTTPServer` (one thread per request) plus
daemon threads for the simulator, the MQTT listener and the WebSocket broadcaster. The
shared `REAL_MQTT` client is guarded by a lock (`state.MQTT_LOCK`) and the MQTT listener
reconnects only on real disconnects (not on idle timeouts). The C core runs a headless
ops loop in the main thread with worker threads for discovery/MQTT; cross-thread
communication goes through the event bus.

## Conversation / topic flow (subscriber map)

| Topic | Publisher | Consumer |
|-------|-----------|----------|
| `tsn/cmd/<id>/*` | web GUI / any CNC | firmware agent |
| `tsn/ack/<id>`, `tsn/status` | firmware agent | web GUI (`,` later any CNC) |
| `tsn/discover` | firmware agent | web GUI + plugin |
| `tsn/lwt/<id>` | firmware agent (retained will) | web GUI |
| `tsn/sensors`, `tsn/sensors/<id>/...` | firmware agent | web GUI |
| `tsn/ptp` | firmware agent (gPTP) | web GUI |
| `tsn/fx/cmd/<id>`, `tsn/fx/data` | web GUI / node | firmware agent / nodes |
