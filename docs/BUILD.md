# Build Instructions

## Prerequisites (Debian/Ubuntu example)

```bash
sudo apt update
sudo apt install -y build-essential cmake libsqlite3-dev \
  libmosquitto-dev libopen62541-dev
```

## Configure and build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)
./build/wtsn-configurator
```

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

## Tests

```bash
cmake --build build --target wtsn-tests && ./build/wtsn-tests
```

## Notes for developers

- Code is C11.
- No comments in code, all documentation lives in `docs/`.
- New protocol integrations go in `src/<protocol>` and register via
  `PluginManager` / MVC event bus.
