"""
WTSN micro:bit display panel (MicroPython) - wired UART link
==========================================================
Connects to the ESP32 agent over a serial wire and prints sensor-style lines that the
ESP republishes onto the WTSN MQTT bus (tsn/sensors), so the webgui can show them.

Wiring:  micro:bit PIN0 --> ESP GPIO14 (UART RX)   (3.3V logic)
         micro:bit GND --> ESP GND

The display uses the capability sensor button A/B to switch what is shown.

Run this with MicroPython (mu / python.microbit.org / v2).
"""

from microbit import *

def send(line: str):
    """Emit a line on the serial output; the ESP picks it up on its RX pin."""
    print(line)

# ---- buttons switch which value is displayed ----
idx = 0
val = "."
last = 0.0

while True:
    # simulate/echo a value; on a real setup you would read a sensor here.
    # We just send a sample that the ESP forwards as telemetry.
    send("mb:" + str(idx) + ":" + str(last))
    last += 0.1

    if button_a.was_pressed():
        idx = (idx + 1) % 3
    if button_b.was_pressed():
        idx = (idx + 2) % 3

    if idx == 0:
        display.show("T")
    elif idx == 1:
        display.show("H")
    else:
        display.show("L")
    sleep(700)
