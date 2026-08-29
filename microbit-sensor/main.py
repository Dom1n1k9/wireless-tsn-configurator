"""
WTSN micro:bit V2 display panel (MicroPython)

Receives the sensor JSON an ESP32 WTSN node sends over UART
(ESP TX=GPIO17 -> micro:bit P0/RX, share GND) and shows it on the LED matrix.
No WiFi/MQTT on the micro:bit — it is a wired display + sound peripheral.

Controls (per the request):
  - Button A : next value   (temperature -> humidity -> light ->)
  - Button B : previous value (cycles the other way)
  - Movement : PIR from the ESP is NOT shown on the matrix; it is only indicated
              by a beep on the built-in V2 speaker.

Incoming frame (115200 baud, newline-terminated JSON):
{"id":"...","sensors":[{"sensor_id":"temp1","type":0,"value":22.4,"unit":"C"},
{"sensor_id":"hum1",...},{"sensor_id":"light1",...},{"sensor_id":"pir1",...}]}
"""
import json
import time
from microbit import *
from music import pitch

# ESP TX -> micro:bit P0 as RX. micro:bit's default UART TX is P0; we force RX=P0
# (and move unused TX to P2) so an external 3.3V UART line from the ESP is heard.
uart.init(tx=pin2, rx=pin0, baudrate=115200)

# latest known values: {sensor_id: (value, unit)}
LATEST = {}


def parse(line):
    global LATEST
    try:
        obj = json.loads(line)
    except Exception:
        return
    vals = {}
    for s in obj.get("sensors", []):
        sid = s.get("sensor_id")
        if sid:
            vals[sid] = (s.get("value"), s.get("unit", ""))
    if vals:
        LATEST.update(vals)
        event = obj.get("event")
        if event == "init":
            display.show(Image.HEART, wait=False)


# cycle order over the numeric/environment values (not the motion indicator)
ORDER = ["temp1", "hum1", "light1"]
INDEX = 0


def render():
    key = ORDER[INDEX] if INDEX < len(ORDER) else "temp1"
    if key in LATEST:
        value, unit = LATEST[key]
        if unit:
            display.scroll("%s%s" % (value, unit), delay=90, wait=False)
        else:
            display.scroll(str(value), delay=90, wait=False)
    else:
        display.show("?", wait=False)


while True:
    if uart.any():
        line = uart.readline()
        if line:
            parse(line.decode("utf-8", "replace").strip())

    if button_a.was_pressed():
        INDEX = (INDEX + 1) % len(ORDER)
        render()
    if button_b.was_pressed():
        INDEX = (INDEX - 1) % len(ORDER)
        render()

    # motion is only signalled by sound (do not replace the chosen display)
    if ("pir1" in LATEST) and (LATEST["pir1"][0] or 0) == 1:
        try:
            pitch(880, 200)
        except Exception:
            pass
        time.sleep(0.3)

    time.sleep(0.05)
