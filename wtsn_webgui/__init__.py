"""WTSN Configurator - web GUI package (Python, paho-mqtt for MQTT).

Deployment vs Simulation mode, devices with TSN functions and grandmaster/slave
role, QoS (802.1Q, priority 0-7), VLAN (ID), TAS/GCL (802.1Qbv), gPTP
(802.1AS), sensors, MQTT, OPC UA FX over MQTT (FXMQTT / C2C Field Exchange),
and a live network/frame monitor. In simulation mode a background thread
fabricates devices, sensors and a realistic frame flow so everything can be
exercised without HW.

Run:  python3 webgui.py [--host H] [--port P]   ->  http://127.0.0.1:8000/

Deployment options (env also accepted):
  --host / --port   bind address and port (default 127.0.0.1:8000)
  WTSN_Host         bind address; use 0.0.0.0 to expose on the network
  WTSN_PORT         port
  WTSN_DB           real-mode sqlite path
  WTSN_BROKER       MQTT broker address host:port (default 127.0.0.1:1883)
  WTSN_USER/PASS    broker auth
  WTSN_WEB_USER/PASS  optional HTTP Basic auth for the web UI
TLS is not bundled: put it behind a reverse proxy (nginx/caddy) for production.
"""

__version__ = "1.0.0"
