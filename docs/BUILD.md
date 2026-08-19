# Build Instructions

## Prerequisites (Debian/Ubuntu example)

```bash
sudo apt update
sudo apt install -y build-essential cmake libsqlite3-dev \
  libmosquitto-dev libopen62541-dev
```

### Without root: build dependencies into `$HOME/local`

The project builds with GCC; you only need cmake to configure and the three run-time
libraries (SQLite3, mosquitto, open62541). When you cannot `apt install` (no sudo),
build them into `$HOME/local` and point the build at them:

```bash
export PKG_CONFIG_PATH=$HOME/local/lib/pkgconfig
export LD_LIBRARY_PATH=$HOME/local/lib

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$HOME/local \
  -DWTSN_ENABLE_PUBSUB=ON
cmake --build build -- -j$(nproc)
```

## Configure and build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)
./build/wtsn-configurator
```

> **Important:** cmake and the three libraries must be on `PATH`/`LD_LIBRARY_PATH` at
> run time if installed locally (`export LD_LIBRARY_PATH=$HOME/local/lib`).

## Options

- `-DBUILD_GUI=ON`/`OFF` - build LVGL front-end (default ON)
- `-DBUILD_PLUGINS=ON`/`OFF` - build sample plugins (default ON)
- `-DWTSN_DB_PATH` - override default sqlite database path
- `-DWTSN_ENABLE_PUBSUB=ON` - use real OPC UA PubSub. Requires open62541 that
  was itself built/installed with PubSub enabled (`-DUA_ENABLE_PUBSUB=ON`).
  Without it the app falls back to the simulated loopback PubSub backend.

## Running

```bash
# GUI mode (default)
./build/wtsn-configurator

# headless / CLI mode
./build/wtsn-configurator --headless --db ./cfg.db
```

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
