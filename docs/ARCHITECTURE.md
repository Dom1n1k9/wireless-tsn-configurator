# Wireless TSN Configurator - Architecture

## Overview

Pure C, modular, MVC-based desktop application. All persistent state is stored in SQLite.

```
                     ┌────────────────────────────────────────────┐
                     │                    GUI (LVGL)             │
                     │  Dashboard | Devices | TSN | VLAN | Time   │
                     │  Sync | OPC UA | MQTT | Settings           │
                     └──────────────────┬─────────────────────────┘
                                        │ events / commands
                     ┌──────────────────▼─────────────────────────┐
                     │             MVC Controller                  │
                     │   routes UI events -> service layer        │
                     │   pushes model notifications -> views      │
                     └──────────────────┬─────────────────────────┘
                                        │
        ┌───────────────────────────────┼───────────────────────────────┐
        ▼                               ▼                               ▼
┌───────────────┐  ┌───────────────┐  ┌───────────────┐  ┌───────────────────┐
│ Device Manager│  │ QoSMgr VLAN   │  │  TimeSyncMgr  │  │  SensorManager    │
│               │  │ TAS/GCL       │  │               │  │                   │
└───────┬───────┘  └───────────────┘  └───────────────┘  └─────────▲─────────┘
        │                                    │                     │
        ▼                                    ▼                     │
┌───────────────┐  ┌───────────────┐  ┌───────────────┐  ┌─────────┴─────────┐
│ Discoverers   │  │ OPC UA Server │  │  MQTT Client  │  │  MQTT↔OPC Gateway │
│  MQTT / OPCUA │  │ (open62541)   │  │ (mosquitto)   │  │  bidirectional    │
└───────┬───────┘  └───────────────┘  └───────────────┘  └───────────────────┘
        ▼
┌───────────────┐
│ Plugin Manager│  loadable .so protocol plugins
└───────────────┘
        ▼
┌───────────────┐
│   SQLite DB   │  devices, qos, vlan, tas, timesync, sensors
└───────────────┘
```

## Layers

1. **Common** (`src/common`) - logging, string utilities, error handling.
2. **Model/MVC** (`src/mvc`) - Model base, event bus, controller, view base.
3. **Database** (`src/db`) - SQLite schema init and CRUD repositories.
4. **Services** (`src/device`, `src/qos`, `src/vlan`, `src/timesync`,
   `src/tas`, `src/sensors`) - domain logic.
5. **Discovery** (`src/discovery`) - discover MQTT/OPC UA devices.
6. **Protocols** (`src/mqtt`, `src/opcua`, `src/gateway`) - network layers.
7. **Plugins** (`src/plugin`) - loadable protocol plugins.
8. **UI** (`src/ui`) - LVGL based pages + theme.

## Data Flow

- UI page -> `AppController` -> service -> repository -> SQLite
- Service -> event bus -> UI view updates (device status changes)
- Discoverer -> DeviceManager -> DB (persisted, restored on startup)

## Plugin Architecture

Plugins expose `discover`, `read`, `write`, `probe` functions described by the
`wtsn_plugin_api.h` interface. Discoverers are plugins; the discovery framework
loads them at startup and enumerates discovered nodes.

## Threading Model

- Main thread: GUI event loop.
- Worker threads: discovery pollers, MQTT callbacks, OPC UA server, gateway.
- Cross-thread communication via thread-safe event bus (mutex + queue).
