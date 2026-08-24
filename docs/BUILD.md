# Build Instructions

## Prerequisites (Debian/Ubuntu example)

```bash
sudo apt update
sudo apt install -y build-essential cmake libsqlite3-dev \
  libmosquitto-dev
```

### Without root: build dependencies into `$HOME/local`

The project builds with GCC; you only need cmake to configure and the run-time
libraries (SQLite3, mosquitto). When you cannot `apt install` (no sudo),
build them into `$HOME/local` and point the build at them:

```bash
export PKG_CONFIG_PATH=$HOME/local/lib/pkgconfig
export LD_LIBRARY_PATH=$HOME/local/lib

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$HOME/local
cmake --build build -- -j$(nproc)
```

## Configure and build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)
./build/wtsn-cli --headless # C CLI core
python3 ../webgui.py        # Python web GUI front-end (from repo root: python3 webgui.py)
```

> **Important:** cmake and the required libraries must be on `PATH`/`LD_LIBRARY_PATH` at
> run time if installed locally (`export LD_LIBRARY_PATH=$HOME/local/lib`).

## Options

- `-DBUILD_GUI=ON`/`OFF` - install the Python web GUI (`webgui.py`) (default ON)
- `-DBUILD_PLUGINS=ON`/`OFF` - build sample plugins (default ON)
- `-DWTSN_DB_PATH` - override default sqlite database path

## Web GUI deployment

`webgui.py` is a single-file, stdlib-only Python web server:

```bash
python3 webgui.py --host 0.0.0.0 --port 8000
```

Security notes:

- Default bind is `127.0.0.1` (loopback only). Use `--host 0.0.0.0` to
  expose, but then set HTTP Basic auth via env `WTSN_WEB_USER`/`WTSN_WEB_PASS`.
- There is no TLS built in — put it behind a reverse proxy (nginx/caddy) for TLS.
- MQTT broker address via `WTSN_BROKER=host:port`; broker auth via
  `WTSN_USER`/`WTSN_PASS`.
- Real-mode commands are only published when the mode is switched to **Real**.

## Packaging

```bash
cmake --build build --target package   # uses CPack
cpack -G TGZ                          # or ZIP on Windows
```

Installs `wtsn-cli`, `tsn-node-simulator`, `tsn-node-agent`, plugins,
`webgui.py`, profiles and docs into `bin` / `share/wtsn-configurator`.

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

The Linux/Raspberry Pi adapter applies QoS/VLAN via `iproute2`+`tc` and maps
time sync to `ptp4l`/`phc2sys`. The ESP32/STM32/NXP adapters are
compile-safe stubs ready to be filled with the vendor SDK.

## Tests

```bash
cmake --build build --target wtsn-tests && ./build/wtsn-tests
```

## Notes for developers

- Code is C11.
- No comments in code, all documentation lives in `docs/`.
- New protocol integrations go in `src/<protocol>` and register via
  `PluginManager` / MVC event bus.
