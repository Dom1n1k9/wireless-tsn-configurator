# Changelog

All notable changes to the WTSN Configurator. Firmware releases bump
`WTSN_FW_VERSION` in `shared/wtsn_version/wtsn_version.h`; host releases are
tagged the same way.

## [Unreleased]

### Documentation
- Reorganized the README into a single, consistent reference (quick start,
  architecture, components, provisioning, protocol, security, build/test,
  layout, FAQ) with a table of contents.
- Rewrote `docs/ARCHITECTURE.md` (removed the dead GUI controller/view, fixed
  the FX topic map, added the web GUI package structure and threading model).
- Rewrote `docs/BUILD.md` (dropped the non-existent `WTSN_DB_PATH` option,
  documented `WTSN_TLS_*`, CTest incl. `test_json_host`, ASan/cppcheck as
  blocking, action-handler development).
- Updated `esp32-agent/README.md` to match the actual firmware: NVS
  `ap_pass`/broker-auth keys, the real FX (`tsn/fx/cmd`, `tsn/fx/data`) and
  telemetry topic table, UART CRC-16/CCITT framing, default
  `wtsn-broker.local` broker, `WTSN_DEVICE_ID` override.

### Changed (UI)
- The web GUI *Domains* page is now clearly labelled as **organizational**:
  it stores the per-cell grouping and device→domain assignment, but deployment
  (`exec_all`) is still global and does not yet scope by domain. Same note
  added to the README and `docs/ARCHITECTURE.md`.

## [1.1.0] — 2026-09-02

### Fixed
- Web GUI `set_mode` action now actually stores the selected mode
  (simulation/real) instead of discarding the value, and rejects unknown values.
- Latent `NameError` in the config rollback token-replay path (`re` was only
  imported as `_re`).
- 20+ C translation units had missing `#include` directives (latent
  implicit-declaration bugs on strict compilers); all fixed.

### Added
- **Shared ESP-IDF components** (`shared/`):
  - `wtsn_prov` — one provisioning implementation (SoftAP + HTTP portal,
    per-board SSID `WTSN-Setup-<id>`, re-provision fallback) used by both
    `esp32-agent` and `esp32-cam`; removes ~250 lines of duplicated code.
  - `wtsn_version` — single `WTSN_FW_VERSION` constant; both firmwares now
    report `"fw"` in `tsn/discover` and `tsn/status` payloads.
- **Windows launcher** `run.ps1` (mirror of `run.sh`): LAN IP detection,
  optional mosquitto startup, GUI auto-start with a self-healing health
  monitor, browser launch. `-Headless` for services only.
- **Python tests** (`tests/test_webgui.py`): 22 unit + HTTP smoke tests.
- **C tests**: `str_util`, `event_bus` and `config version` coverage added to
  `wtsn-tests` (now 82 tests).
- **CI** (`.github/workflows/ci.yml`): Python job (ruff + unittest),
  AddressSanitizer/UBSan build job, cppcheck job, alongside the existing
  build/test/package job.
- **ESP-IDF CI** (`.github/workflows/esp.yml`): builds both firmwares on
  IDF v5.3 and uploads the binaries as artifacts.
- REST-style action routing: `POST /api/actions/<name>` (the legacy
  `/api/action` endpoint is kept for backward compatibility); unknown actions
  return a proper error instead of a silent no-op.
- Constant-time comparison for the web GUI basic-auth check.

### Changed
- `webgui.py` (1,700-line single file) is now a thin shim over the
  `wtsn_webgui/` package (state, MQTT broker, DB, MQTT link, simulator,
  actions, HTTP server, static UI split out).
- Simulator log/string helpers now shim over the shared `common/log.h` and
  `common/str_util.h` (duplicate implementations removed).
- Web GUI static UI moved to `wtsn_webgui/static/index.html`.

### Removed
- Dead `src/app/main_gui.c` (never built, superseded by `webgui.py`).
- Duplicated provisioning code in `esp32-cam` (see shared `wtsn_prov`).
- 157 vendored files under `esp32-cam/managed_components/` and the generated
  `esp32-cam/sdkconfig` are no longer tracked in git (ESP-IDF manages them at
  build time); both are gitignored.

### Security
- Documented the plaintext-provisioning trade-off (open SoftAP, plain-HTTP
  portal) in the main README and the agent README; see
  *Security note: plaintext provisioning*.

## [1.0.0]

Initial public release: C11 control-plane core, SQLite persistence, Python web
GUI with simulation/real modes, generic node simulator, ESP32 firmware agent
with zero-touch provisioning, ESP32-CAM MJPEG node, micro:bit sensor panel,
FXMQTT integration, CI build/test/package.
