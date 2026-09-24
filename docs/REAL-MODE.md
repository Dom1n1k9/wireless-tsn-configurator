# WTSN Real-Mode Runbook (hardware day)

From "ESP32 + relay in hand" to "real device in the GUI, config deployed,
relay clicking". The Pi is already the complete CNC: broker, C core, web GUI,
edge AI, local LLM, backups, auto-update.

Credentials (MQTT pass, WiFi PSK, GUI login):
`/home/dominik/Documents/wtsn-site-credentials.txt` — never paste them here.

---

## 0. Already done (no action needed)

- Pi services all active (verified incl. reboot test):
  `mosquitto wtsn-cli wtsn-webgui wtsn-ai wtsn-policy wtsn-llm tailscaled`
- mosquitto listens on `0.0.0.0:1883`, password auth — a LAN publish from the
  PC was tested OK, so the agent will be able to reach it
- Firmware built and current (rebuild only if firmware sources changed):
  - agent: `esp32-agent/build/wtsn_esp32_agent.bin`
  - cam:   `esp32-cam/build/wtsn_esp32_cam.bin` (optional)
  - rebuild: `. ~/esp/esp-idf/export.sh && idf.py build` in the project dir

## 1. Parts

| item | notes |
|------|-------|
| ESP32 DevKit (agent) | the relay board auto-detects as `esp32-02` |
| Relay module (optocoupler) | terminals: `6-30V  GND  Trigger  GND_T` + USB |
| 12 V DC supply | for the relay coil (accepts 6–30 V) — **verify voltage before connecting** |
| USB data cable, jumpers | |
| PC with ESP-IDF | `~/esp/esp-idf` (already set up) |

## 2. Flash the agent

```bash
cd esp32-agent
. ~/esp/esp-idf/export.sh
idf.py -p /dev/ttyUSB0 flash monitor     # adjust /dev/ttyUSB*
```

- Onboard LED blinks **3×** = flash/reboot confirmed.
- You can leave it USB-powered; provisioning uses the board's own radio.

## 3. Provision (first boot only)

1. Board boots with no WiFi in NVS → SoftAP **`WTSN-Setup-<id>`**
   (unique per board).
2. Join it from a phone/PC, open **http://192.168.4.1/**.
3. Fill in:
   - Device ID — leave empty for auto-detect (BME280 on I²C → `esp32-01`,
     otherwise `esp32-02`), or type one explicitly.
   - WiFi SSID — `T-266581`
   - WiFi password — from the credentials file
   - MQTT broker host:port — **`192.168.1.248:1883`** (Pi's LAN IP; the board
     cannot use `localhost`)
   - MQTT user / MQTT password — `wtsn` / from the credentials file
     (leave empty only for an anonymous broker)
4. Save → board reboots, joins T-266581, connects to the broker.

Sanity check on the PC:

```bash
mosquitto_sub -h 192.168.1.248 -p 1883 -u wtsn -P <pass> -t 'tsn/#' -v
# expect: tsn/discover {…}, then periodic tsn/status heartbeats
```

## 4. Switch the GUI to real mode

- GUI top-right mode switch → **Real** (or `POST /api/actions/set_mode
  {"mode":"real"}`).
- **The mode is in-memory only** — after any webgui restart it returns to
  Simulation; flip it again.
- Real mode uses a **separate database** (`wtsn_gui.db`); the sim demo config
  (VLAN/QoS/streams seeded in `wtsn_sim.db`) does not carry over. For the
  hardware demo, configure directly in real mode (a few QoS rows + a stream is
  enough), or keep it minimal: device → deploy → ping → actor.

## 5. Verify the device end-to-end

1. **Devices page** — the real device appears automatically from its
   `tsn/discover` (id, firmware, IP, RSSI), status online.
2. **Ping** → ack lands (RTT sample appears in Metrics).
3. **Identify** (Devices → identify) → LED blinks on the board.
4. Add one QoS row (or a stream), then header → **Execute settings on
   controller** → deploy ack; the deploy column goes green
   (`last_deploy_ok=1`).
5. **OTA round-trip**: Firmware page → upload `wtsn_esp32_agent.bin`
   (select the real device for kind-compat) → Flash. The agent downloads
   `http://192.168.1.248:8000/fw/<file>`, verifies the **CRC32** against the
   uploaded value, reboots into the new app; firmware version updates in the
   GUI. A CRC mismatch aborts and the old app stays active (boot pointer is
   reverted to the running partition).

## 6. Relay wiring + actuation

```
ESP32 GPIO26 ──> Trigger
ESP32 GND    ──> GND_T          (common ground is mandatory)
12V supply + ──> 6-30V
12V supply − ──> GND
USB          ──> powers the relay's logic section (if present)
```

- Firmware drives **GPIO26 HIGH = relay ON**. Do NOT connect the coil supply
  to the USB 5 V rail.
- The relay command is `tsn/cmd/<id>/actor` with a timer-switch mode `0–6`;
  **ON modes are 1, 2, 3, 6** (use `1` for simple on). Ack:
  `{"id":…,"ok":true,"mode":1,"prev":0}`.
- The GUI actor page is on hold, so drive it directly for the demo:

```bash
mosquitto_pub -h 192.168.1.248 -p 1883 -u wtsn -P <pass> \
  -t 'tsn/cmd/esp32-02/actor' -m '1'     # ON  (you should hear it click)
mosquitto_pub -h 192.168.1.248 -p 1883 -u wtsn -P <pass> \
  -t 'tsn/cmd/esp32-02/actor' -m '0'     # OFF
```

- **If the relay doesn't click:** GPIO26 is 3.3 V logic; classic 5 V-trigger
  optocoupler relays (SRD-type) may not turn on at 3.3 V. Check the USB logic
  power first; if still dead, add a small level shift (e.g. 2N2222 with base
  resistor ~1 kΩ from GPIO26, collector to Trigger, emitter to GND_T).

## 7. Optional: ESP32-CAM

```bash
cd esp32-cam && idf.py -p /dev/ttyUSB0 flash monitor
```

Provision identically (Device ID `esp32-cam`). It appears with kind = camera;
AI-triggered clips then show on the Devices page and in the edge clip store.

## 7b. Optional: SSD1306 OLED + 4 buttons on esp32-02 (actor board)

The `esp32-agent` firmware includes a small built-in SSD1306 (128x64, I2C)
driver plus four debounced push-buttons, enabled automatically on the actor
board (esp32-02 - no BME280). No extra component download needed.

Wiring (module has `SCL SDA VCC GND` + `K1..K4`):

| OLED module | esp32-02 |
|-------------|----------|
| VCC | 3V3 |
| GND | GND |
| SCL | GPIO22 (I2C clock - shared bus) |
| SDA | GPIO21 (I2C data - shared bus) |
| K1 | GPIO32 (to GND on press) |
| K2 | GPIO33 (to GND on press) |
| K3 | GPIO34 (to GND on press) |
| K4 | GPIO35 (to GND on press) |

- I2C runs at 100 kHz on the same bus as the sensor add-on; internal pull-ups
  are enabled. For K3/K4 (GPIO34/35 are input-only, no internal pull-up) add
  a 10 kΩ pull-up to 3V3, or rely on the switch module's own pull.
- Rebuild + flash esp32-02: `. ~/esp/esp-idf/export.sh && cd esp32-agent && idf.py -p /dev/ttyUSB0 flash`.

Behaviour:
- The OLED shows the device id and a two-line status; the bottom row shows the
  live K1..K4 levels.
- Each button press publishes `tsn/button/<id>/K<n>` and an `tsn/ack/<id>` and
  is reported as a sensor `btn1..btn4` (`tsn/sensors`) so it appears on the
  GUI Sensors page.
- Set the display text over MQTT:
  `mosquitto_pub -h <pi> -u wtsn -P <pass> -t tsn/cmd/esp32-02/display -m '{"line1":"LIVE","line2":"OK"}'`
- Read a button level: `... -t tsn/cmd/esp32-02/button -m '1'`

There is also a **Display** button per device row in the GUI (Devices page)
that opens a small dialog to set line1/line2 over MQTT.

## 8. Factory reset / re-provision

- **Hold BOOT (GPIO0) ~3 s** → NVS erased → back to the `WTSN-Setup` AP.
- Or over MQTT: `tsn/cmd/<id>/factory` payload `1`.
- Fallback: if a provisioned board cannot reach its saved WiFi, the
  provisioning AP comes back on its own — re-provision without reflashing.

## 9. Back to simulation

GUI mode → Simulation. The deterministic 7-device sim fleet resumes on
`wtsn_sim.db`; the real device's rows live in `wtsn_gui.db` and are untouched.

## Troubleshooting

| symptom | check |
|---------|-------|
| No `WTSN-Setup` AP | power OK? hold BOOT 3 s (factory reset) to re-enter provisioning |
| Joins WiFi, no MQTT traffic | broker must be the **Pi LAN IP** (192.168.1.248:1883), user/pass correct; watch `mosquitto_sub -t 'tsn/#'` |
| Device row offline | LWT fired — board lost the broker; check `tsn/status` cadence in Monitor |
| Deploy fails | read the ack line in Monitor (`tsn/ack/<id>`); confirm the device id matches the command topic |
| OTA stuck/rolled back | serial monitor: CRC mismatch or HTTP error is logged; old app stays active |
| Relay silent | 3.3 V level (see §6), coil supply voltage, common ground |
| Board has wrong id | portal Device ID field, or factory reset + re-provision |

## Hardware-day demo checklist

- [ ] Agent flashed, LED 3× blink
- [ ] Provisioned (T-266581 + broker + creds), `tsn/discover` seen
- [ ] GUI in Real mode, device online with fw/IP/RSSI
- [ ] Ping ack + identify blink
- [ ] Config deploy → green deploy status
- [ ] OTA round-trip, firmware version bumped
- [ ] Relay ON/OFF via actor command (click audible)
