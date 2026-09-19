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
 ┌───────────────────────────── RPi edge node ─────────────────────────────┐
 │  ┌──────────────────────────────────────────────────────────────────┐   │
 │  │              Web GUI (Python wtsn_webgui, :8000)                 │   │
 │  │ Devices | Monitor | Metrics | Sensors | AI | Architecture |      │   │
 │  │ FXMQTT | Timesync | QoS | VLAN | TAS | Preemption | Streams      │   │
 │  │ sim (virtual fleet + ACKs + FX data)   real (MQTT link)  actions │   │
 │  └───────────────┬───────────────────────────────┬──────────────────┘   │
 │                  │ HTTP/WS + MQTT topics         │ llm_chat (HTTP :8081)│
 │  ┌───────────────▼───────────────┐   ┌───────────▼─────────────────┐    │
 │  │  C11 control-plane core       │   │  Edge AI (rpi-ai/)          │    │
 │  │  app (composition root)       │   │  vision_service.py          │    │
 │  │  └─ managers: device, qos,    │   │    YOLOv4 on CAM stream     │    │
 │  │    vlan, timesync, tas,       │   │  policy_engine.py           │    │
 │  │    stream, sensors, domain,   │   │    autonomous rules R1..R3  │    │
 │  │    config_version, radio      │   │  llm_bridge.py              │    │
 │  │  └─ mvc: model + event bus    │   │    Ollama -> allowlist ->   │    │
 │  │  └─ db: SQLite + repos        │   │    GUI action API           │    │
 │  └───────────────┬───────────────┘   └───────────┬─────────────────┘    │
 │  ┌───────────────▼───────────────┐   ┌───────────▼─────────────────┐    │
 │  │  mosquitto (MQTT broker)      │   │  Ollama (qwen2.5:1.5b)      │    │
 │  └───────────────┬───────────────┘   └─────────────────────────────┘    │
 └──────────────────┼──────────────────────────────────────────────────────┘
                    │  MQTT
      ┌─────────────┼───────────────────────────────┐
      ▼             ▼                               ▼
  tsn-node-agent  tsn-node-simulator           ESP32 agent / CAM
  host (Linux/RPi) virtual nodes               (ESP-IDF firmware,
  iproute2 + tc    profiles/*.ini              zero-touch provisioning,
                                               gPTP, sensors, OTA A/B)
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
- Vision: CAM MJPEG → YOLOv4 detection → `tsn/sensors/event` + ai_* sensors →
  policy engine (R1) / GUI Sensors page
- Policy engine: telemetry + DB state → rule (cooldown-checked) → GUI action API
  (`source="ai"`) → same path as a human change → AI Decisions audit row
- LLM chat: GUI `llm_chat` → bridge → Ollama → JSON proposal → allowlist/clamp
  validation → GUI action API (`source="llm"`) → AI Decisions audit row

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
| `state.py` | shared mutable state + locks (events, acks, mode, DB paths, MQTT client lock) |
| `db.py` | SQLite schema, versioned migrations, event trace, loaders, history |
| `mqtt_broker.py` | paho wrapper: synchronous, thread-safe broker surface (with optional TLS via `WTSN_TLS_*`) |
| `mqtt_link.py` | real-mode broker cache + background listener loop (status/ack/discover/LWT/sensors) |
| `sim.py` | simulation engine — stable virtual fleet, drifting sensors, FX data, stream-status transitions, and simulated per-device deploy ACKs (timers) |
| `actions/` | per-domain action handlers (devices, qos, vlan, tas, timesync, streams, fxmqtt, misc incl. versions/backup/`llm_chat`) behind a thin dispatcher |
| `server.py` | HTTP server, JSON API, hand-rolled WebSocket, basic auth, firmware serving + upload (CRC32), `llm_chat` proxy to the bridge |
| `static/index.html` | single-file SPA (plain JS, no framework/build step) |

**Simulation model.** In *sim* mode no MQTT is used at all: `sim.py` owns a stable
virtual fleet (ESP32 sensor/relay boards, ESP32-CAM, STM32, Linux) and, each tick,
writes device rows (upsert, so per-device columns like `last_deploy_at`/`last_deploy_ok`
survive), sensor samples, FX field-exchange rows (`fx_data`) and metrics. *Execute
settings on controller* in sim mode reuses the exact same per-device snapshot builder as
real mode, marks every device "deploy pending" and then fires a `threading.Timer` per
device (150–600 ms) that lands a realistic ACK (DB + `RECENT_ACKS` + event + WebSocket);
a short wait + one retry pass covers stragglers. This makes the whole deploy/ACK/
retry/status flow exercisable end-to-end without hardware.

**Firmware & OTA.** `server.py` serves `build/fw/` over HTTP and an upload endpoint that
validates the file type, computes the **CRC32**, derives a version from the filename and
records it in the `firmware` table (with device kind). The OTA action publishes
`{"url","size","crc32"}` on `tsn/cmd/<id>/ota`; the ESP32 verifies the CRC device-side
before rebooting (see `shared/wtsn_ota`).

**LLM proxy.** The `llm_chat` action forwards the chat to the local LLM bridge
(`WTSN_LLM_URL`, default `127.0.0.1:8081`) and renders the (allowlist-validated) executed
actions inline. The GUI holds no model weights — it is a thin client of the bridge.

**Threading model.** The web GUI uses `ThreadingHTTPServer` (one thread per request) plus
daemon threads for the simulator, the MQTT listener and the WebSocket broadcaster. The
shared `REAL_MQTT` client is guarded by a lock (`state.MQTT_LOCK`) and the MQTT listener
reconnects only on real disconnects (not on idle timeouts). The C core runs a headless
ops loop in the main thread with worker threads for discovery/MQTT; cross-thread
communication goes through the event bus.

## Edge AI (`rpi-ai/`)

Three standalone Python services that run next to the GUI on the Pi and share its MQTT
broker + SQLite DB. They deliberately have **no direct network authority**: they act by
calling the GUI's own action API (which applies the same validation and persists the same
state as a human operator would), and they tag each change with a provenance.

| Service | Entry point | Loopback HTTP | Behaviour |
|---------|-------------|---------------|-----------|
| `wtsn-ai` | `vision_service.py` | — | Pulls the ESP32-CAM MJPEG stream, runs **YOLOv4-tiny** (OpenCV DNN, 80 COCO classes). On a target (default `person`): publishes a motion event on `tsn/sensors/event` (the CAM firmware records its own microSD clip), publishes `ai_detect`/`ai_person` counters as sensors, keeps a rolling thumbnail + short clip |
| `wtsn-policy` | `policy_engine.py` | — | Polls DB/MQTT state and applies cooldowned rules through the GUI API: **R1** person+PIR → raise QoS + open TAS gate; **R2** grandmaster gPTP offset too high → switch grandmaster to the best-offset node; **R3** E2E latency too high → reserve an 802.1Qcc stream. Config: `policy.json` |
| `wtsn-llm` | `llm_bridge.py` | `127.0.0.1:8081` | Front-end for **Ollama** (default `qwen2.5:1.5b`). Chat → strict-JSON proposal → **allowlist + clamp + known-device validation** → GUI action API with `source="llm"` → result back to chat. The model can only propose from a fixed list (`save_qos`, `save_vlan`, `save_stream`, `deploy_stream`, `ping_device`, `exec_all`, …) |

**Provenance & audit.** Every configuration change carries a `source` (`user`, `ai`,
`llm`). The **AI Decisions** table records time, source, device, action, params and
reason; the Devices page flags devices being configured by AI. Any AI/LLM change is
reversible via **Config Versions**.

**Safety boundaries.**
- The LLM has no socket access of its own — it is a *proposal* engine; the bridge is the
  only thing with a GUI credential, and it enforces the allowlist.
- Policy rules are rate-limited by per-rule cooldowns and fully logged.
- Vision is perception-only: it emits events/counters; it never reconfigures by itself
  (that is the policy engine's job).
- All three are loopback/localhost-bound; Ollama and the LLM bridge are not exposed.

## Conversation / topic flow (subscriber map)

| Topic | Publisher | Consumer |
|-------|-----------|----------|
| `tsn/cmd/<id>/*` | web GUI / any CNC | firmware agent |
| `tsn/ack/<id>`, `tsn/status` | firmware agent | web GUI (later any CNC) |
| `tsn/discover` | firmware agent | web GUI + plugin |
| `tsn/lwt/<id>` | firmware agent (retained will) | web GUI |
| `tsn/sensors`, `tsn/sensors/<id>/...` | firmware agent, **vision service** (ai_* counters) | web GUI, **policy engine** |
| `tsn/sensors/event` | firmware agent (PIR / WiFiVision), **vision service** (clip trigger) | web GUI, **policy engine**, ESP32-CAM (records clip) |
| `tsn/ptp` | firmware agent (gPTP) | web GUI, **policy engine** (R2) |
| `tsn/fx/cmd/<id>`, `tsn/fx/data` | web GUI / node | firmware agent / nodes |
| GUI action API (`/api/actions/*`) | **policy engine**, **LLM bridge** (as `ai`/`llm`) | web GUI (applies with provenance) |
