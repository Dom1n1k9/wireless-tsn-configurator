# Build Instructions

## Prerequisites (Debian/Ubuntu example)

```bash
sudo apt update
sudo apt install -y build-essential cmake libsqlite3-dev libmosquitto-dev
```

### Without root: build dependencies into `$HOME/local`

The project builds with GCC; you only need cmake to configure and the run-time
libraries (SQLite3, mosquitto). When you cannot `apt install` (no sudo), build them
into `$HOME/local` and point the build at them:

```bash
export PKG_CONFIG_PATH=$HOME/local/lib/pkgconfig
export LD_LIBRARY_PATH=$HOME/local/lib

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$HOME/local
cmake --build build -- -j$(nproc)
```

> **Important:** cmake and the required libraries must be on `PATH`/`LD_LIBRARY_PATH`
> at run time if installed locally (`export LD_LIBRARY_PATH=$HOME/local/lib`).

## Configure and build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)
./build/wtsn-tests     # C tests
./build/wtsn-cli --headless   # C CLI core
python3 webgui.py      # Python web GUI front-end (from repo root)
```

The default database path for `wtsn-cli` is `wtsn.db` (override with `--db <path>`).

## Options

- `-DBUILD_GUI=ON`/`OFF` — install the Python web GUI (`webgui.py`) (default ON)
- `-DBUILD_PLUGINS=ON`/`OFF` — build the sample MQTT discovery plugin (default ON)
- `-DBUILD_TESTING=ON`/`OFF` — enable CTest (default ON; also builds the host JSON
  parser test `test_json_host`)

## Web GUI deployment

`webgui.py` is a thin shim over the `wtsn_webgui/` package (stdlib-only aside from
`paho-mqtt`):

```bash
python3 webgui.py --host 0.0.0.0 --port 8000
```

Environment variables (also usable via the GUI):

| Env | Purpose |
|-----|---------|
| `WTSN_HOST`, `WTSN_PORT` | bind address/port (default `127.0.0.1:8000`) |
| `WTSN_DB` | real-mode SQLite path (default `build/wtsn_gui.db`) |
| `WTSN_BROKER` | MQTT broker address `host:port` (default `127.0.0.1:1883`) |
| `WTSN_USER` / `WTSN_PASS` | MQTT broker auth |
| `WTSN_TLS_CA`, `WTSN_TLS_CERT`, `WTSN_TLS_KEY` | MQTT TLS (CA bundle + optional client cert) |
| `WTSN_TLS_INSECURE` | `1`/`true` to skip broker certificate verification (dev only) |
| `WTSN_WEB_USER` / `WTSN_WEB_PASS` | optional HTTP Basic auth for the web UI |

Security notes:

- Default bind is `127.0.0.1` (loopback only). Use `--host 0.0.0.0` to expose, but then
  set HTTP Basic auth via `WTSN_WEB_USER`/`WTSN_WEB_PASS`.
- There is no built-in TLS in the HTTP server — put it behind a reverse proxy
  (nginx/caddy) for HTTPS.
- Real-mode MQTT commands are only published when the GUI is switched to **Real** mode.

## Packaging

```bash
cmake --build build --target package   # uses CPack
cpack -G TGZ                          # or ZIP on Windows
```

Installs `wtsn-cli`, `tsn-node-simulator`, `tsn-node-agent`, plugins, `webgui.py`,
profiles and docs into `bin` / `share/wtsn-configurator`.

## TSN node firmware agent

```bash
cmake --build build --target tsn-node-agent
./build/tsn-node-agent --id node-01 --platform linux --mqtt-host localhost --mqtt-port 1883
```

| Option         | Description                          |
|----------------|--------------------------------------|
| `--id`         | Device id reported to the configurator |
| `--platform`   | `linux`, `raspberry_pi`, `esp32`, `stm32`, `nxp` |
| `--mqtt-host`  | Configurator / broker host (default `localhost`) |
| `--mqtt-port`  | MQTT port (default 1883)            |

The Linux/Raspberry Pi adapter applies QoS/VLAN via `iproute2` + `tc` and maps time sync
to `ptp4l`/`phc2sys`. The ESP32/STM32/NXP adapters are compile-safe stubs; the real
ESP32 implementation lives in `esp32-agent/` (ESP-IDF).

## Tests

```bash
# all C tests via CTest (includes the host JSON parser test)
cmake --build build
(cd build && ctest --output-on-failure)

# or just the main suite
./build/wtsn-tests

# Python web GUI tests + lint
python3 -m unittest discover -s tests
python3 -m ruff check webgui.py wtsn_webgui tests
```

### Sanitizers & static analysis

```bash
# AddressSanitizer/UBSan (+ leak detection)
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 (cd build-asan && ctest --output-on-failure)

# cppcheck (blocking in CI)
cppcheck --enable=warning,performance,portability --std=c11 --language=c -Isrc \
  --suppress=missingIncludeSystem \
  src/common src/mvc src/db src/device src/domain src/config_version \
  src/qos src/vlan src/tas src/sensors src/stream src/mqtt src/fxmqtt \
  src/telemetry src/trace src/plugin src/app
```

These two are enforced by CI (`.github/workflows/ci.yml`) — the baseline must stay clean.

## Notes for developers

- Code is C11, built with `-Wall -Wextra`.
- New protocol integrations go in `src/<protocol>` and register via the plugin manager /
  event bus.
- The `wtsn_webgui/actions/` package splits handlers per TSN domain; add a new action by
  placing it in the relevant `HANDLERS` dict.
