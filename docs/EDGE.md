# Raspberry Pi Edge Deployment

How the WTSN Configurator is deployed as a self-running **edge node** on a
Raspberry Pi (reference: RPi 5, 4 GB, Raspberry Pi OS 64-bit / Debian). Everything —
broker, CNC core, web GUI, edge AI, local LLM, auto-update and backup — runs on the
Pi; the ESP32 nodes are pure wireless clients.

```
RPi edge node (user: wtsn)
├── /home/wtsn/wtsn-configurator     git checkout (repo; auto-updated)
├── /home/wtsn/wtsn-configurator/build/   C core binaries + wtsn_sim.db / wtsn_gui.db + fw/
├── /home/wtsn/wtsn-ai               edge AI scripts (synced from rpi-ai/) + policy.json + logs
├── /home/wtsn/wtsn.db               wtsn-cli headless controller DB
├── /home/wtsn/backups/<stamp>/      daily backups (14-day retention)
└── /etc/wtsn/env                    ALL secrets (root-only, 0600)
```

## What runs (systemd)

| Unit | Kind | Purpose |
|------|------|---------|
| `mosquitto` | service | MQTT broker with username/password auth (`/etc/mosquitto/`) |
| `wtsn-cli` | service | C11 CNC core, headless (`--db /home/wtsn/wtsn.db`) |
| `wtsn-webgui` | service | Web GUI on `0.0.0.0:8000` (Basic auth) |
| `wtsn-ai` | service | Vision: YOLOv4-tiny on the ESP32-CAM stream (`rpi-ai/vision_service.py`) |
| `wtsn-policy` | service | Autonomous TSN rules R1–R3 (`rpi-ai/policy_engine.py`) |
| `wtsn-llm` | service | LLM bridge :8081 → Ollama → allowlisted GUI actions (`rpi-ai/llm_bridge.py`) |
| `ollama` | service | Local LLM runtime (default model `qwen2.5:1.5b`, CPU) |
| `wtsn-update` | service + **timer** (30 min) | `git pull` → C core rebuild → sync `rpi-ai/*.py` → restart changed services |
| `wtsn-backup` | service + **timer** (daily) | hot SQLite copies + key configs → `/home/wtsn/backups/` |
| `tailscaled` | service | stable remote address (optional but recommended) |

The unit files live in [`rpi-ai/systemd/`](../rpi-ai/systemd/). They contain **no
secrets** — every service reads `EnvironmentFile=/etc/wtsn/env`.

## Setup

### 1. Base packages

```bash
sudo apt update
sudo apt install -y mosquitto mosquitto-clients git cmake build-essential \
  libsqlite3-dev libmosquitto-dev python3 python3-pip avahi-daemon
sudo python3 -m pip install paho-mqtt opencv-python-headless numpy
```

Create the service user (no password login; the GUI/MQTT passwords live in
`/etc/wtsn/env`, not in the OS):

```bash
sudo useradd -m -s /bin/bash wtsn
sudo mkdir -p /etc/wtsn && sudo touch /etc/wtsn/env && sudo chmod 600 /etc/wtsn/env
sudo chown root:root /etc/wtsn/env
```

### 2. Repository + first build

```bash
sudo -u wtsn git clone https://github.com/<you>/wtsn-configurator /home/wtsn/wtsn-configurator
sudo -u wtsn bash -c 'cd /home/wtsn/wtsn-configurator \
  && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build -j4'
```

### 3. Secrets — `/etc/wtsn/env`

```bash
sudo tee /etc/wtsn/env > /dev/null <<'EOF'
# Web GUI (HTTP Basic auth)
WTSN_WEB_USER=<gui-user>
WTSN_WEB_PASS=<gui-password>
# MQTT broker auth (same credentials on the agents via NVS)
WTSN_USER=<mqtt-user>
WTSN_PASS=<mqtt-password>
# Auto-update: GitHub token (fine-grained, contents:read on this repo only)
WTSN_GH_TOKEN=<token>
# Local LLM
WTSN_OLLAMA_MODEL=qwen2.5:1.5b
EOF
```

> Never commit these values. The repo and the systemd units stay credential-free.

### 4. MQTT broker with auth

`/etc/mosquitto/conf.d/wtsn.conf` (file present → `password_file` required by
mosquitto; create it with `mosquitto_passwd`):

```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd <mqtt-user>
```

```
listener 1883
allow_anonymous false
password_file /etc/mosquitto/passwd
```

### 5. Install the units

```bash
for u in wtsn-cli wtsn-webgui wtsn-ai wtsn-policy wtsn-llm \
         wtsn-update.service wtsn-update.timer wtsn-backup.service wtsn-backup.timer; do
  sudo cp rpi-ai/systemd/$u /etc/systemd/system/
done
sudo systemctl daemon-reload
sudo systemctl enable --now mosquitto wtsn-cli wtsn-webgui wtsn-ai wtsn-policy wtsn-llm
sudo systemctl enable --now wtsn-update.timer wtsn-backup.timer
```

The edge AI scripts are placed by the auto-update (`update.sh` copies
`rpi-ai/{vision_service.py,policy_engine.py,llm_bridge.py,update.sh,backup.sh}` into
`/home/wtsn/wtsn-ai/`); for a first manual run:

```bash
mkdir -p /home/wtsn/wtsn-ai && cd /home/wtsn/wtsn-configurator
cp rpi-ai/vision_service.py rpi-ai/policy_engine.py rpi-ai/llm_bridge.py \
   rpi-ai/update.sh rpi-ai/backup.sh /home/wtsn/wtsn-ai/
sudo chown -R wtsn:wtsn /home/wtsn/wtsn-ai
```

The policy engine reads optional thresholds from `/home/wtsn/wtsn-ai/policy.json`
(all keys optional — defaults are sane):

```json
{ "r1": { "enabled": true },
  "r2": { "offset_ns": 500, "cooldown_s": 300 },
  "r3": { "latency_ms": 50, "min_samples": 3, "cooldown_s": 300 } }
```

### 6. Local LLM (Ollama)

```bash
sudo apt install -y ollama        # or curl -fsSL https://ollama.com/install.sh | sh
sudo systemctl enable --now ollama
sudo -u wtsn ollama pull qwen2.5:1.5b
```

The LLM bridge (`wtsn-llm`) listens on `127.0.0.1:8081` and Ollama on
`127.0.0.1:11434` — both loopback-only, never exposed.

### 7. Multi-WiFi failover (NetworkManager)

The Pi should try the site network first and fall back to others. One
`nmconnection` per network, ordered by `connection.priority` (higher wins):

```bash
sudo nmcli connection add type wifi con-name wtsn-<net1> ifname wlan0 \
  ssid "<SSID-1>" wifi-sec.key-mgmt wpa-psk \
  802-11-wireless-security.psk '<pass1>' \
  connection.autoconnect yes connection.autoconnect-priority 100
sudo nmcli connection add type wifi con-name wtsn-<net2> ifname wlan0 \
  ssid "<SSID-2>" wifi-sec.key-mgmt wpa-psk \
  802-11-wireless-security.psk '<pass2>' \
  connection.autoconnect yes connection.autoconnect-priority 50
```

`wlan0` then follows the best available network automatically (and a
`netplan-wlan0-*` profile, if present, keeps the lowest priority). The connection
files land in `/etc/NetworkManager/system-connections/` (root-only) — **do not copy
them into the repo**; keep the PSKs out of git and out of backups.

### 8. Tailscale (optional, recommended)

Stable reachability regardless of which WiFi the Pi sits on:

```bash
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up --hostname rpi
```

Then from any of your devices: GUI at `http://rpi:8000`, SSH as `ssh wtsn@rpi`.

## Operations

```bash
# service state
systemctl status wtsn-webgui wtsn-cli wtsn-ai wtsn-policy wtsn-llm
# auto-update log (pull/build/restart outcomes)
tail -f /home/wtsn/wtsn-ai/update.log
# run an update by hand
sudo -u wtsn /home/wtsn/wtsn-ai/update.sh
# timers
systemctl list-timers | grep wtsn
# latest backups
ls -t /home/wtsn/backups | head
# journal per service
journalctl -u wtsn-webgui -n 50 --no-pager
```

- **Auto-update** runs every 30 min: `git pull --ff-only` → `cmake --build` →
  sync the five `rpi-ai/*.py|sh` scripts → `systemctl restart` the five services.
  It only acts when HEAD actually changed; a failed pull keeps the old code.
- **Backup** runs daily: hot copies of the SQLite DBs (`build/wtsn_sim.db`,
  `build/wtsn_gui.db`, `/home/wtsn/wtsn.db`) plus key config files, into
  `/home/wtsn/backups/<YYYYmmdd-HHMMSS>/`, 14-day retention.
- **Restoring a backup** = stop `wtsn-webgui`/`wtsn-cli`, copy the DB files back
  into place, restart. The schema migrates forward on startup (`ensure_schema`).

## Security notes

- **`/etc/wtsn/env`** is the single secret store (0600 root:root); systemd units,
  the repo and the backups never contain credentials.
- The web GUI is exposed with **HTTP Basic auth** (`WTSN_WEB_USER`/`WTSN_WEB_PASS`);
  for HTTPS, put a reverse proxy (nginx/caddy/Tailscale funnel) in front.
- The MQTT broker requires **username/password** (agents carry the same credentials
  in NVS: `muser`/`mpass`).
- Edge AI ports are **loopback-only**; the LLM can only execute allowlisted,
  clamped actions through the GUI API (provenance `llm`) — it has no network access.
