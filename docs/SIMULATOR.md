# TSN Node Simulator

A **generic** simulator of TSN-capable controllers. It intentionally does **not**
emulate a specific platform (e.g. real ESP32 silicon). Instead, it models
arbitrary virtual nodes defined by configuration profiles, so you can simulate
ESP32, Raspberry Pi, STM32, NXP, Linux or any custom node type in a uniform way.

It complements the Wireless TSN Configurator by populating the network with
virtual devices that speak the same protocols.

## Features

- Device Discovery (announcements)
- MQTT publishing of node state
- OPC UA endpoint (per profile, planned)
- QoS (IEEE 802.1Q priority 0-7, traffic class, bandwidth, latency)
- VLAN (VLAN ID 1-4094 + group)
- Time Synchronization (slave / master / external grandmaster, gPTP, 802.1AS)
- TAS (cycle time, deploy target)
- Gate Control Lists (open/closed windows)
- Sensor Simulation (temperature, pressure, IMU, distance, GPIO)

## Profiles

Profiles are `.ini` files in `profiles/`. See the bundled `esp32.ini`,
`rpi.ini`, `stm32.ini`, `nxp.ini`, `linux.ini`.

Sections and keys:

| Section       | Key                    | Meaning                            |
|---------------|------------------------|------------------------------------|
| `[device]`    | `id`, `name`, `kind`  | Identifier, display name, type     |
|               | `firmware`, `model`    | Version / hardware model           |
| `[network]`   | `ip`, `mac`, `mqtt_topic` | Addressing                       |
| `[capabilities]`| `tsn_features`       | Colon-separated list, e.g. `qbv:802.1as:qos:vlan` |
| `[services]`  | `qos`, `vlan`, `timesync`, `tas`, `sensors` | on/off |
| `[qos]`       | `priority`, `traffic_class`, `bandwidth_kbps`, `latency_ms` | QoS |
| `[vlan]`      | `vlan_id`, `group`     | VLAN membership                  |
| `[timesync]`  | `mode`, `grandmaster`, `protocol`, `offset_ns` | Sync |
| `[tas]`       | `cycle_time_ns`, `deploy_target` | TAS cycle                |
| `[gcl]`       | `<index>` = `open\|closed:<duration_ns>` | Gate windows   |
| `[sensors]`   | `<index>` = `<type>:<id>:<unit>:<value>` | Sensors       |

## Build

```bash
cmake --build build --target tsn-node-simulator
```

## Run

```bash
# all bundled profiles with MQTT + OPC UA
./build/tsn-node-simulator --all

# specific profiles, custom broker
./build/tsn-node-simulator \
  --profile profiles/esp32.ini \
  --profile profiles/nxp.ini \
  --mqtt-host 192.168.1.10 --mqtt-port 1883 \
  --opcua-base 4840 --once
```

| Option            | Description                             |
|-------------------|-----------------------------------------|
| `--profile <file>`| Add a device profile (repeatable)        |
| `--all`           | Load all bundled profiles                |
| `--mqtt-host`     | MQTT broker host (default `localhost`) |
| `--mqtt-port`     | MQTT broker port (default 1883)        |
| `--opcua-base`    | Base OPC UA port (default 4840)        |
| `--once`          | Run a single tick and exit              |
| `--help`          | Show usage                             |

Without any `--profile`, all bundled profiles are loaded.

## Architecture

```
profiles/*.ini  ->  sim_profile_load()  ->  sim_device (model)
                                              |
                          sim_simulator (collection + tick loop)
                              |  |  |
              discovery/   mqtt/    opcua/
            announce()  publish  endpoints
```

The simulator core (`sim_simulator`) ticks every node: it wobbles sensor
values and advances TAS gate state from the GCL. The protocol layer then
exposes the node state over discovery, MQTT and OPC UA.
