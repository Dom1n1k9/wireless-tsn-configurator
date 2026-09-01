"""
WTSN micro:bit display panel (MicroPython) - wired UART link
==========================================================
The ESP32 agent pushes its sensor values to the micro:bit once a second on
P1 (RX):  "T:21.5 P:1013.2 H:48 L:1234 M:0 A:0"
The panel shows ONE value at a time with its unit (T:27C, P:1006hPa, H:48%,
L:918lx, M:1) and HOLDS it until B (next value) or A (previous) is pressed.
When the ESP PIR reports motion (M:1 rising edge) it flashes a heart and
beeps (piezo buzzer on P16 + GND; MicroPython has no built-in speaker API).

Wiring:  ESP GPIO15 (TX) --> micro:bit P1 (RX)
         micro:bit P0 (TX) --> ESP GPIO14 (RX)   (own sensors, optional)
         GND --> GND
         piezo + --> P16, piezo - --> GND

Run with the MicroPython port for micro:bit V2 (python.microbit.org).
"""

from microbit import display, button_a, button_b, sleep, Pin, Image

try:
    from machine import UART
except ImportError:
    from microbit import UART

BAUD = 115200
uart = UART(1, BAUD, tx=Pin(0), rx=Pin(1))
BUZZ = Pin(16, Pin.OUT)


def beep():
    """Short 880 Hz beep on the piezo (P16)."""
    try:
        BUZZ.tone(880)
        sleep(0.3)
        BUZZ.off()
    except AttributeError:
        # port without tone(): crude ~440 Hz square wave
        for _ in range(260):
            BUZZ.toggle()
            sleep(0.0011)
        BUZZ.off()


# ---- latest values received from the ESP agent ----
vals = {"T": 0, "P": 0, "H": 0, "L": 0, "M": 0, "A": 0}


def parse_line(line):
    for tok in line.split(" "):
        key, sep, num = tok.partition(":")
        if sep and key in vals:
            try:
                vals[key] = float(num)
            except ValueError:
                pass


def read_line():
    if uart.any() > 0:
        data = uart.readline()
        if data:
            parse_line(data.decode().strip())


# ---- display views, in B/A cycle order: T -> P -> H -> L -> M ----
VIEWS = 5
view = 0
prev_pir = 0


def render():
    """Show the current value with its unit; display.show blocks ~2 s."""
    if view == 0:
        display.show("T:%dC" % int(vals["T"]))
    elif view == 1:
        display.show("P:%dhPa" % int(vals["P"]))
    elif view == 2:
        display.show("H:%d%%" % int(vals["H"]))
    elif view == 3:
        display.show("L:%dlx" % int(vals["L"]))
    else:
        display.show("M:%d" % int(vals["M"]))


while True:
    if button_b.was_pressed():
        view = (view + 1) % VIEWS
        render()
    if button_a.was_pressed():
        view = (view + VIEWS - 1) % VIEWS
        render()
    read_line()
    if vals["M"] == 1 and prev_pir == 0:
        display.show(Image.HEART, delay=1500)
        beep()
    prev_pir = vals["M"]
    # hold the current view; re-render after the ~2 s display + short gap
    sleep(3)
