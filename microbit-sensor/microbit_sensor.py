"""
WTSN micro:bit sensor endpooint (MicroPython)
===========================================

Publishes readings from sensors attached to the micro:bit onto the same MQTT
sub-system the WTSN webgui (webgui.py) listens to.

The → tsn/sensors topic uses the exact JSON shape the webgui parses:
    {"id": "<device_id>", "sensors": [ {"sensor_id":..., "type":..., "value":..., "unit":...} ]}

Sensor types (webgui): 0=temperature, 1=pressure, 2=IMU/accelerometer,
3=distance, 4=GPIO/voltage/logical.

Edit BROKER / device id below to match your network.
"""

import network
import time
import json
import math

# ---- config ----
SSID = "your-wifi-ssid"
PASS = "your-wifi-password"
BROKER = "192.168.0.149"      # PC running the WTSN configurator / webgui
PORT = 1883
DEVICE_ID = "microbit-01"

# micro:bit pin mapping for the MAKER:bit / breakout edge connector
P0 = pin0   # noqa: F821 - MicroPython built-ins
P1 = pin1   # noqa: F821
P2 = pin2   # noqa: F821

import umqtt.simple as mqtt   # noqa: E402  (bundled with MicroPython)

wlan = network.WLAN(network.STA_IF)
wlan.active(True)

if not wlan.isconnected():
    wlan.connect(SSID, PASS)
    for _ in range(30):
        if wlan.isconnected():
            break
        time.sleep(1)
print("IP:", wlan.ifconfig()[0])
if not wlan.isconnected():
    raise SystemExit("Could not connect to WiFi")


def read_temperature():
    """internal MCU temperature via the on-chip temperature sensor (rough)."""
    # P0 analog read doubles as a crude temp on some boards; use TMP36 on P0 instead:
    #   v = P0.read_analog()/1023*3.3 ; return (v-0.5)*100
    return None


c = mqtt.MQTTClient(DEVICE_ID, BROKER, PORT)
c.connect()

print("publishing sensor telemetry on tsn/sensors ...")

while True:
    ambient = P1.read_analog()        # 0..1023  -> 0..3.3V
    voltage = ambient / 1023 * 3.3    # V
    lookup = P2.read_digital()          # PIR / button / light digital pin

    payload = {
        "id": DEVICE_ID,
        "sensors": [
            {"sensor_id": "microbit_mcu_v", "type": 4, "value": round(voltage, 2), "unit": "V", "healthy": 1},
            {"sensor_id": "microbit_light", "type": 4, "value": ambient, "unit": "LSB", "healthy": 1},
            {"sensor_id": "microbit_logical", "type": 4, "value": lookup, "unit": "", "healthy": 1},
        ],
    }
    c.publish("tsn/sensors", json.dumps(payload))
    display.show("W")                # noqa: F821 - activity LED   # noqa: F821
    time.sleep(3)
    display.clear()                  # noqa: F821
