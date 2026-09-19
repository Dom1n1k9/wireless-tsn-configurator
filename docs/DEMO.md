# WTSN Configurator — 5-minute demo script

Run from the Pi in **Simulation mode**. Credentials (GUI / MQTT / Tailscale):
`/home/dominik/Documents/wtsn-site-credentials.txt` (PC) — never put them here.

- GUI: `http://rpi:8000` or `http://192.168.1.248:8000`
- Seeded state: 7 devices, all deployed; QoS on 3; VLAN 100 "Cameras"; 2 TAS
  schedules; stream `st-demo` running; grandmaster `esp32-01`; firmware v1.2.0;
  ~9 config versions; live FX, metrics, sensors, AI clips.
- LLM steps take **~5–20 s** warm (CPU inference; the first call after boot
  or a service restart adds ~10–20 s model load) — that is the "watch it
  think" moment. Everything else is instant.

---

## 1. Open (30 s)

Open the GUI. Header shows the **SIM** badge; Devices lists 7 online nodes.

> "This is the WTSN Configurator running on the Raspberry Pi edge node.
> Right now it simulates a 7-device TSN fleet; in real mode the exact same
> GUI talks to physical ESP32 agents over MQTT — nothing else changes."

## 2. Devices (45 s)

- Point at: status, RSSI, USB, firmware, and the green **deploy** column.
- Click **Ping** on `esp32-01` → a `PING`/ack line appears in the Monitor
  feed and a latency sample lands in Metrics.
- Click the `esp32-cam` row → **▶ AI clip** → plays an MJPEG clip recorded on
  the edge (thumbnail `last.jpg` shows the last detection).

> "Every node reports heartbeat, signal and deploy status. The camera node
> records AI-triggered clips locally and you can replay them right here."

## 3. QoS + VLAN (45 s)

- **IEEE 802.1Q → QoS Priority**: `esp32-01` prio 4, `esp32-02` prio 5,
  `esp32-cam` prio 6 (traffic class, bandwidth, latency budget).
- **WVLAN ID**: group *Cameras*, VLAN 100, members `esp32-cam` + `esp32-03`.

> "Per-device 802.1Q priority and latency budgets, with the camera traffic
> isolated in its own VLAN."

## 4. TAS + Preemption (30 s)

- **802.1Qbv TAS**: *Camera schedule* — 8 ms cycle, target `esp32-01`,
  two-gate GCL; plus *AI motion gate*.
- **802.1Qbu Preemption**: `esp32-01` on, eMAC 6 / pMAC 3,4.

> "Time-aware shaping gives the camera window guaranteed bandwidth;
> preemption lets express traffic cut in."

## 5. Streams (30 s)

- **802.1Qcc TSN Streams**: `st-demo`, talker `esp32-01` → listeners
  `esp32-02`, `stm32-01`, status **running** (deployed).

> "A zero-point stream with latency/interval budgets, pushed to every
> listener — note the deploy status."

## 6. Time sync + Metrics + Monitor + Sensors (45 s)

- **802.1AS**: grandmaster `esp32-01` + slave nodes.
- **Metrics**: E2E control-plane latency chart (the ping you sent shows up)
  and gPTP offset/jitter history.
- **Monitor**: live event feed — BMCA elections, ACKs, telemetry, PING.
- **Sensors**: per-sensor sparklines (temp, pressure, light, PIR, sonar,
  WiFi-motion, AI detections).

> "This is the control-plane telemetry: how fast commands round-trip and
> how healthy the gPTP clock is — all on the edge."

## 7. FXMQTT (30 s)

- Broker config + the **live FX data card** (rows appear as field exchange
  happens between simulated nodes).

> "OPC UA field exchange carried over MQTT — the C2C data channel between
> nodes, with the live feed right here."

## 8. AI Assistant — the money shot (60 s)

- **Ask a question**: `how do I configure this network from scratch?`
  → after a few seconds: a step-by-step guide, **nothing changed**
  (no new entry in AI Decisions).
  > "The assistant is a local 1.5b model on the Pi — no cloud. Ask it how
  > something works and it just explains."
- **Order a change**: `set esp32-03 priority to 4`
  *(repeatable: pick any device not yet listed on the QoS page)*
  → after ~5–20 s: `save_qos` **executed + auto-deployed**; a new row
  appears on the QoS page and in **AI Decisions** (source `llm`).
  > "Order it to make a change and it does — inside a strict allowlist of
  > validated actions, then deploys automatically."

## 9. Config Versions (30 s)

- List shows the snapshots (`demo-baseline`, `demo-tuned`, …).
- Click a version → **diff** shows exactly what changed (including what the
  AI just did); mention rollback is one click.

> "Everything is snapshotted. This diff shows precisely what the assistant
> changed — and you can roll it back."

## 10. Closer (15 s)

- Header: **Execute settings on controller** → deploys ALL config; every
  device ACKs; deploy column all green.

> "One button pushes the whole configuration to every node in the network."

---

## If something looks off

```bash
# services (all must be active)
systemctl is-active mosquitto wtsn-cli wtsn-webgui wtsn-ai wtsn-policy wtsn-llm tailscaled
# re-seed the demo state (walkthrough script on the PC at /tmp/opencode/walkthrough.py,
# scp it to the Pi and: python3 /tmp/walkthrough.py)
# journal for a service
journalctl -u wtsn-webgui -n 50 --no-pager
```
