# Wireless TSN Configurator

> **Centralized controller (CNC) for Wireless Time-Sensitive Networking (W-TSN).**
> Manage ESP32 / Raspberry Pi / STM32 / NXP / Linux nodes against **IEEE 802.1Qcc**
> (QoS, VLAN, gPTP/time-sync, TAS/GCL, stream reservation), exposed over **OPC UA FX
> over MQTT (FXMQTT)**. **Zero-touch onboarding**: flash an agent, power it on, and it
> provisions and connects itself. **Local AI on the edge**: a Raspberry Pi runs a
> local LLM assistant, an autonomous policy engine and camera-based vision — all
> decisions are validated, executed with provenance and auditable.

A production-oriented configuration and control plane for W-TSN. The **control-plane
core is written in pure C (C11)** and ships as a CLI/headless service, a host firmware
agent, and a generic node simulator. The **front-end is a Python web GUI**
(`webgui.py` / `wtsn_webgui/`) with a single-file, dependency-light SPA. The **edge AI
services** (`rpi-ai/`) run on the Raspberry Pi next to the GUI.

It acts as a centralized controller (CNC-style, aligned with IEEE 802.1Qcc) that
discovers wireless nodes, manages them, applies QoS / VLAN / time-synchronization /
schedule policies, reads sensors, performs firmware OTA with CRC-verified images and
exposes the whole network over **FXMQTT** — OPC UA FX / C2C Field Exchange carried
over MQTT. Sienda Time Sensitive Network Stack will be implemented.

---

## Table of contents

1. [Quick start](#quick-start)
2. [Architecture at a glance](#architecture-at-a-glance)
3. [Components](#components)
   - [C core (CLI / headless)](#c-core)
   - [Web GUI](#web-gui)
   - [Edge AI services](#edge-ai-services)
   - [Firmware agents](#firmware-agents)
   - [Simulator](#simulator)
4. [Raspberry Pi edge deployment](#raspberry-pi-edge-deployment)
5. [AI on the edge](#ai-on-the-edge)
6. [Firmware & OTA](#firmware--ota)
7. [Provisioning & onboarding](#provisioning--onboarding)
8. [MQTT / FXMQTT protocol](#mqtt--fxmqtt-protocol)
9. [Security](#security)
10. [Build & test](#build--test)
11. [Project layout](#project-layout)
12. [Requirements](#requirements)
13. [FAQ / notes](#faq--notes)
14. [License](#license)

---

## Quick start

Everything runs from one machine. See [Raspberry Pi edge deployment](#raspberry-pi-edge-deployment)
for the full Pi service setup, and [Provisioning & onboarding](#provisioning--onboarding)
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
 ┌──────────────────────────── RPi edge node ───────────────────────────┐
 │                                                                      │
 │   ┌────────────────────────────────────────────────────────────┐     │
 │   │            Web GUI (Python wtsn_webgui, :8000)             │     │
 │   │ Devices | Monitor | Metrics | Sensors | AI | Architecture  │     │
 │   │ FXMQTT | Timesync | QoS | VLAN | TAS | Preemption | Streams│     │
 │   └───────────────┬──────────────────────────────┬─────────────┘     │
 │                   │ HTTP/WS + MQTT               │ llm_chat (HTTP)   │
 │   ┌───────────────▼──────────────┐   ┌───────────▼───────────────┐   │
 │   │  C11 control core (src/)     │   │ Edge AI (rpi-ai/)         │   │
 │   │  managers: device, qos, vlan,│   │ vision (YOLOv4 on CAM)    │   │
 │   │  timesync, tas, stream, ...  │   │ policy engine (R1..R3)    │   │
 │   │  SQLite │ MQTT/FXMQTT │ trace│   │ LLM bridge (Ollama, allow-│   │
 │   └───────────────┬──────────────┘   │ list-validated actions)   │   │
 │                   │                  └───────────────────────────┘   │
 │   ┌───────────────▼──────────────┐   ┌───────────────────────────┐   │
 │   │ mosquitto (MQTT broker)      │   │ Ollama (qwen2.5:1.5b)     │   │
 │   └───────────────┬──────────────┘   └───────────────────────────┘   │
 └───────────────────┼──────────────────────────────────────────────────┘
                     │ MQTT
      ┌──────────────┼──────────────────────────────┐
      ▼              ▼                              ▼
 esp32-agent /   tsn-node-agent             tsn-node-simulator
 esp32-cam       (host Linux/RPi agent)     (virtual nodes, profiles)
 (ESP-IDF, zero-touch provisioning,
  gPTP, sensors, WiFiVision, OTA A/B)
```

- **C core** (`src/`) — modular, dependency-injected managers connected through an
  event bus; all state persisted in **SQLite**; communicates only via **MQTT/FXMQTT**.
- **Web GUI** (`wtsn_webgui/`) — Python (mostly stdlib) HTTP + WebSocket front-end
  with a **Simulation** and a **Real** mode.
- **Edge AI** (`rpi-ai/`) — vision, policy engine and LLM bridge; runs next to the
  GUI on the Pi, talks to the GUI through its action API (see [AI on the edge](#ai-on-the-edge)).
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
| Devices    | add/remove/ping nodes; status, **deploy status** (last apply + ACK per device), USB port, domain; **per-device firmware manager** (upload / flash / OTA with CRC), **live camera** (ESP32-CAM), TSN features, **config versions / rollback** |
| Monitor    | live network/frame trace with search filter, **Pause/Start**, Clear |
| Metrics    | control-plane E2E latency + gPTP clock offset/jitter history, per-device summary + SVG charts (legend, axis labels, time windows) |
| Sensors    | live values per node + **history sparklines** (temperature, pressure, humidity, light, PIR, sonar, **WiFi CSI motion**, **AI detection**) |
| AI Assistant | **local LLM chat** — executes validated TSN actions or guides configuration step by step; **AI Decisions** audit trail with provenance (AI / LLM / user) |
| Architecture | live network topology: nodes, links and live traffic |
| FXMQTT     | Field Server / Participant, broker address, **live FX data feed** (C2C field exchange values) + test-send |
| Synchronization | gPTP grandmaster / slave setup (802.1AS), per-node offset reports |
| QoS / VLAN | 802.1Q priority mapping (priority 0–7, traffic class, bandwidth, latency), VLAN groups + membership |
| TAS / GCL  | gate control lists with **visual gate windows** (802.1Qbv) |
| Preemption | eMAC / pMAC priority split (802.1Qbu) |
| Streams    | 802.1Qcc talker / listener reservations, deploy, status (ready / standby / failed) |

Run it directly:

```bash
python3 webgui.py [--host H] [--port P]
# env: WTSN_HOST, WTSN_PORT, WTSN_DB, WTSN_BROKER, WTSN_USER, WTSN_PASS,
#      WTSN_WEB_USER, WTSN_WEB_PASS, WTSN_LLM_URL, WTSN_TLS_CA, WTSN_TLS_CERT, WTSN_TLS_KEY
```

**Modes.** *Simulation* fabricates a stable fleet of nodes (ESP32 sensor/relay boards,
ESP32-CAM, STM32, Linux), sensors, FX field-exchange data and a frame flow — and
**simulates the whole deploy loop**: *Execute settings on controller* publishes a
per-device snapshot, every device ACKs after a realistic delay (with one retry pass
for stragglers) and the Devices page shows each device's deploy status. *Real*
connects to your MQTT broker and live devices (commands are only published in Real
mode).

> **TLS in the GUI.** TLS itself is not bundled — put the GUI behind a reverse proxy
> (nginx/caddy) for HTTPS; MQTT TLS is optional via the `WTSN_TLS_*` env vars above.

### Edge AI services

Three small Python services (`rpi-ai/`) that run on the Raspberry Pi next to the GUI
(one systemd unit each: `wtsn-ai`, `wtsn-policy`, `wtsn-llm`):

| Service | Script | What it does |
|---------|--------|--------------|
| `wtsn-ai` | `vision_service.py` | **YOLOv4-tiny** (OpenCV DNN) person/object detection on the ESP32-CAM MJPEG stream. On detection: triggers the CAM to record its own microSD clip (motion event on `tsn/sensors/event`), publishes live detection counters (`ai_detect` / `ai_person`) as sensors, keeps a rolling thumbnail + short clip |
| `wtsn-policy` | `policy_engine.py` | **Autonomous TSN decisions** — watches the network and reconfigures it through the GUI action API (provenance **AI**): R1 person + PIR motion → raise QoS + open TAS gate; R2 gPTP grandmaster offset too high → switch grandmaster; R3 E2E latency too high → reserve an 802.1Qcc stream. Thresholds + cooldowns in `policy.json` |
| `wtsn-llm` | `llm_bridge.py` | **Local LLM bridge** (HTTP :8081) in front of **Ollama** (default `qwen2.5:1.5b`, runs on the Pi CPU). Turns chat into strict-JSON action proposals, validates them against an **allowlist with clamped params** and executes them via the GUI API (provenance **LLM**). The LLM never touches MQTT/network directly |

All AI activity lands in the **AI Decisions** table (audit trail with source, action,
params, reason) and the **Devices** page flags devices the AI is currently configuring.
Rollback for any AI change: **Config Versions** page. See [AI on the edge](#ai-on-the-edge).

### Firmware agents

- **`esp32-agent/`** — the reference ESP32 (ESP-IDF v5.x) agent:
  - **zero-touch provisioning** — SoftAP `WTSN-Setup-<id>` + portal
    (http://192.168.4.1/), NVS storage, auto re-provision as fallback,
  - **MQTT command execution** — `apply` (JSON snapshot), `qos`, `vlan`, `timesync`,
    `tas`, `stream`, `preemption`, `status`, `wifi`, `fx`, `actor`, `ping`, `identify`,
    `ota` (CRC-verified), `reset`, `reboot`, `factory`,
  - **TSN config persisted to NVS** and restored on reboot,
  - **software gPTP** over UDP multicast (`224.0.1.129`, best-effort, sub-ms on WiFi),
  - **sensors** — BME280 (bit-bang I2C), TEMT6000 light, HC-S501 PIR, ultrasonic sonar,
    **WiFiVision** (device-free motion from RSSI noise + optional WiFi CSI variance —
    no extra hardware, events on the same sink as the PIR); 1 s heartbeat +
    history + sparklines in the GUI,
  - **relay actor** — GPIO16 relay pulse on motion (auto-detected sensor/relay role),
  - **OTA** — A/B slots, **device-side CRC32 verification** of the downloaded image
    before reboot, automatic rollback on bad boot (`shared/wtsn_ota`),
  - **LWT last-will**, **SNTP** time, **LED** status (provisioning = fast blink,
    connecting = blink, online = solid), **factory reset** (BOOT 3 s),
  - **UART panel** — micro:bit V2 display over wired UART with a **CRC-16/CCITT**
    checksum on every frame (`T/P/H/L/M/A` values).
- **`esp32-cam/`** — ESP32-CAM node streaming MJPEG to the Devices page, motion-driven
  **microSD clip recording** (triggered by the vision service or any PIR/WiFiVision
  event), provisioned with the same shared portal, OTA-capable (CRC-verified).
- **micro:bit sensor panel** (`microbit-sensor/`) — MakeCode (`main.ts`) or MicroPython
  (`microbit_sensor.py`) variant; B/A button cycling, beep on PIR motion.

See [esp32-agent/README.md](esp32-agent/README.md) for the full firmware protocol and
wiring tables, and `docs/SIMULATOR.md` for the virtual nodes.

### Simulator

`tsn-node-simulator` — generic virtual TSN nodes from `profiles/*.ini`
(ESP32 / RPi / STM32 / NXP / Linux). See [docs/SIMULATOR.md](docs/SIMULATOR.md).

---

## Raspberry Pi edge deployment

The reference deployment runs **everything on one RPi 5** (the "edge node"):
broker, CNC core, web GUI, edge AI, Ollama, auto-update and backup — with the
ESP32 nodes as pure wireless clients.

| What | Where |
|------|-------|
| mosquitto (MQTT, auth) | `mosquitto.service` |
| C11 CNC core (headless) | `wtsn-cli.service` |
| Web GUI (Basic auth, :8000) | `wtsn-webgui.service` |
| Vision / Policy / LLM bridge | `wtsn-ai`, `wtsn-policy`, `wtsn-llm` |
| Local LLM runtime | `ollama.service` (qwen2.5:1.5b, CPU) |
| Auto-update (git pull → rebuild → restart) | `wtsn-update.timer` (30 min) |
| Backup (hot DB copies + configs, 14 d) | `wtsn-backup.timer` (daily) |
| Stable remote address (Tailscale) | `tailscaled.service` |
| Multi-WiFi failover (NetworkManager, by priority) | `wlan0` |

All secrets live in **`/etc/wtsn/env`** (root-only, 0600) — the systemd units never
contain credentials. The unit files are in [`rpi-ai/systemd/`](rpi-ai/systemd/) and the
step-by-step setup (services, env file, Ollama, Tailscale, multi-WiFi,
update/backup) is in **[docs/EDGE.md](docs/EDGE.md)**.

Highlights:
- **Auto-update** — every 30 min: `git pull` → C core rebuild → sync `rpi-ai/*.py`
  → restart only the services that changed. Failures keep the old code and log to
  `/home/wtsn/wtsn-ai/update.log`.
- **Backup** — daily hot SQLite copies (sim + real DBs) plus key configs into
  `/home/wtsn/backups/<stamp>/`, 14-day retention.
- **Reachable from anywhere** — Tailscale gives the Pi a stable name/IP
  (`http://rpi:8000`, `ssh wtsn@rpi`) independent of which WiFi it sits on.
- **Multi-WiFi** — NetworkManager profiles with priorities (site hotspot first,
  fallbacks after); the Pi re-joins the best available network automatically.

---

## AI on the edge

Three AI paths, all **local** (no cloud), all **audited**:

1. **LLM assistant (chat).** The AI Assistant page proxies to the LLM bridge
   (`WTSN_LLM_URL`, default `http://127.0.0.1:8081`) → Ollama. The model replies with
   strict JSON: either an **action** (e.g. `save_qos`, `save_vlan`, `save_tas`,
   `save_stream`, `deploy_stream`, `ping_device`, `exec_all`, …) or a **guide**
   (step-by-step how-to using the real GUI pages). Actions are validated against an
   allowlist with clamped numeric params and known-device checks; unknown devices,
   out-of-range values and non-allowlisted actions are refused. Execution goes through
   the normal GUI action API with `source="llm"`, so it is indistinguishable from (and
   rolled back like) any manual change.
   On a Pi CPU a chat answer takes ~30 s with the 1.5 B model — the UI shows a
   "thinking" state.
2. **Policy engine (autonomous).** No chat involved: the policy engine watches MQTT
   telemetry + DB state and applies rules with cooldowns — person + PIR → raise QoS /
   open TAS gate (R1); grandmaster offset too high → swap grandmaster to the
   best-offset node (R2); E2E latency too high → reserve an 802.1Qcc stream (R3).
   Config: `policy.json` (thresholds, cooldowns, on/off per rule).
3. **Vision (perception).** YOLOv4-tiny on the ESP32-CAM stream. Detections become
   (a) sensor values in the GUI (Sensors page), (b) a clip-recording trigger on the
   CAM, and (c) the input the policy engine's R1 rule consumes.

Every change from 1 and 2 is recorded in **AI Decisions** (time, source AI/LLM/user,
device, action, params, reason) and shown live on the Devices page (devices get an
**AI** badge while being configured).

---

## Firmware & OTA

The firmware manager lives **per device** on the Devices page:

- **Upload** — `.bin` / `.img` / `.hex`; the server validates the type, computes the
  **CRC32** and derives a version from the filename (`…_v1.2.3.bin`), tags the device
  kind (sensor / cam) and stores it in `build/fw/` + the `firmware` table.
- **Flash / OTA** — publishes `{"url": "http://<host>:8000/fw/<file>", "size": N,
  "crc32": "…"}` on `tsn/cmd/<id>/ota`. The command is only offered when the stored
  firmware's kind matches the device kind.
- **On the device** — the image is downloaded to the inactive A/B partition
  (`esp_https_ota`), then the device **re-reads the partition and verifies the CRC32**
  against the upload-time value: mismatch → the partition is marked invalid, the
  previous app stays active and the GUI never reboots into bad firmware. Match →
  reboot; a bad new app is rolled back automatically by the bootloader on next boot.
  The new version is reported via `tsn/discover` / `tsn/status` and shown per device.
- **Simulation** — flashing is simulated end-to-end (download → CRC verify → report
  new version), so the whole flow can be demonstrated without hardware.

The firmware version constant for agent builds lives in
`shared/wtsn_version/wtsn_version.h` (`WTSN_FW_VERSION`).

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
| `tsn/sensors`, `tsn/sensors/<id>/...`, `tsn/sensors/event` | in | telemetry / history / motion events |
| `tsn/ptp`              | in  | gPTP reports |
| `tsn/fx/cmd/<id>`      | out | FX / C2C field-exchange commands |
| `tsn/fx/data`, `tsn/fx/<id>` | in/out | FX data feed (motion events, field-server samples) |
| `tsn/cmd/<id>/stream` + `tsn/fx/cmd/<id>` | out | 802.1Qcc stream reservation |

Command payload notes:

- `apply` — one JSON snapshot (priority, traffic class, VLAN, preemption, timesync,
  TAS cycle + GCL) — the preferred path used by *Execute settings on controller*.
- `ota` — `{"url": "...", "size": N, "crc32": "<hex>"}`; `crc32` triggers the
  device-side image verification (see [Firmware & OTA](#firmware--ota)).
- `wifi` — `{"ssid": "...", "pass": "..."}` with **optional** `pass` (omitted → the
  agent keeps its stored password).

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
proxy (nginx/caddy). On the Pi deployment all credentials are kept in
`/etc/wtsn/env` (0600, root-only); the systemd units and the repo never contain them.

### AI safety

- The **LLM can only propose allowlisted actions** with clamped parameters; it has no
  direct network/MQTT access — every execution goes through the normal, audited GUI
  action API (`source="llm"`).
- The **policy engine** changes are rate-limited by per-rule cooldowns and fully
  visible in the AI Decisions audit trail; rollback via Config Versions.
- Edge AI is loopback-only: the LLM bridge listens on `127.0.0.1:8081` and Ollama on
  `127.0.0.1:11434` — neither is exposed.

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
[docs/EDGE.md](docs/EDGE.md) for the Raspberry Pi service deployment, and
[docs/SIMULATOR.md](docs/SIMULATOR.md) for the simulator profiles and options.

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
esp32-cam/            ESP-IDF ESP32-CAM firmware (MJPEG stream node, clips)
shared/               shared ESP-IDF components (wtsn_prov, wtsn_ota, wtsn_version)
microbit-sensor/      micro:bit V2 display panel (wired UART, MakeCode + MicroPython)
rpi-ai/               Raspberry Pi edge services
  vision_service.py   YOLOv4-tiny detection on the ESP32-CAM stream (wtsn-ai)
  policy_engine.py    autonomous TSN rules R1..R3 (wtsn-policy)
  llm_bridge.py       Ollama -> allowlisted GUI actions (wtsn-llm)
  update.sh           auto-update (git pull -> rebuild -> sync -> restart)
  backup.sh           daily hot DB + config backups
  systemd/            unit + timer files for the Pi deployment
profiles/             device profile templates (.ini) for the simulator
docs/                 ARCHITECTURE, BUILD, SIMULATOR, EDGE (Pi deployment)
webgui.py             entry-point shim for the web GUI
wtsn_webgui/          Python web GUI package
tests/                Python unit + HTTP smoke tests
launcher/             desktop launcher + autostart
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/BUILD.md](docs/BUILD.md) and
[docs/EDGE.md](docs/EDGE.md).

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
| OpenCV + numpy| any       | Edge AI vision (Pi only)       |
| Ollama        | any       | Local LLM (Pi only, optional)  |

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

- **Simulation is a full closed loop** — *Execute settings on controller* simulates
  per-device ACKs (with retry), the Devices page shows each device's deploy status,
  streams move between ready/standby/failed, and FX data + sensors are generated
  continuously, so every page is alive without hardware.
- **Simulated sensors** drift realistically around stable base values, so the Sensors
  page is alive instead of frozen; history sparklines come from the same feed.
- **Monitor Pause** keeps buffering new frames so pressing Start resumes where you left
  off.
- **The GUI restarts itself** — a watchdog re-checks the web GUI health every 4 s and
  restarts it if it wedges (`/tmp/wtsn_mon.log`).
- **LLM latency** — the assistant runs a 1.5 B model on the Pi CPU; expect ~30 s per
  answer. Use short, concrete requests ("raise esp32-cam priority to 6") for the
  action path, and open-ended questions ("how do I configure this from scratch?")
  for the guide path.
- **UART integrity** — the ESP32 ↔ micro:bit link appends a CRC-16/CCITT checksum to
  every line; corrupted frames are dropped instead of showing wrong values.
- **OTA integrity** — firmware images carry a CRC32; the device verifies it after
  download and refuses to boot unverified images.
- **CI** (GitHub Actions) builds, tests (incl. ASan/UBSan + cppcheck) and packages on
  every push.
- **`tsn/discovery` vs `tsn/discover`** — a few legacy topics still use the old name;
  both are understood.

---

## License

MIT
