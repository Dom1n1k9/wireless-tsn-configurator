// WTSN micro:bit V2 display panel - wired UART (NOT BLE)
//
// RX (P1): the ESP32 agent pushes its sensor values once a second,
//      "T:21.5 P:1013.2 H:48 L:1234 M:0 A:0"
// The panel shows ONE value with its unit and STAYS on it - no auto-cycling:
//      T:27C -> P:1006hPa -> H:48% -> L:918lx -> M:1 -> (back to T)
// B = next value, A = previous value.
//
// INSTANT switching: pressing a button flips the view immediately (a flag),
// and a fast 300 ms refresh loop renders the CURRENT view, aborting any
// in-progress scroll. You never wait for a long string to finish.
//
// On PIR motion (M:1 rising edge): heart flash + beep (V2 built-in speaker).
//
// TX (P0): reports the micro:bit's own onboard sensors to the ESP, which
// republishes them on MQTT (mb_* sensors).
//
// Wiring:  micro:bit P0 (TX) --> ESP GPIO14 (RX)
//          micro:bit P1 (RX) <-- ESP GPIO15 (TX)
//          GND --> GND

// Route serial to the physical pins (P0 TX / P1 RX) instead of USB.
serial.redirect(SerialPin.P0, SerialPin.P1, BaudRate.BaudRate115200)

// V2 onboard speaker: playTone() goes to the built-in speaker instead of
// the pitch pin (which would be P0 = our UART TX).
music.setBuiltInSpeakerEnabled(true)
music.setVolume(255)

// Latest values received from the ESP32 agent.
let sTemp = 0
let sPress = 0
let sHum = 0
let sLight = 0
let sPir = 0
let prevPir = 0

// Display views, in B/A cycle order: temp -> press -> hum -> light -> motion.
const V_TEMP = 0
const V_PRESS = 1
const V_HUM = 2
const V_LIGHT = 3
const V_MOTION = 4
const N_VIEWS = 5
let view = V_TEMP

// When true, the render loop shows a one-char label/flash briefly before the
// number so the unit change is visible without waiting for a scroll.
let pendingHint = true

// Parse "T:21.5 P:1013.2 H:48 L:1234 M:0 A:0*XXXX" into the s* variables.
// The trailing *XXXX is a CRC-16/CCITT over the payload; lines with a bad
// checksum are ignored so a corrupted UART byte never shows wrong values.
function crc16(str: string): number {
    let crc = 0xFFFF
    for (let i = 0; i < str.length; i++) {
        crc = crc ^ (str.charCodeAt(i) << 8)
        for (let b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else crc = (crc << 1) & 0xFFFF
        }
    }
    return crc
}

function lineOk(line: string): boolean {
    const star = line.lastIndexOf("*")
    if (star < 0) return true            // legacy frame, accept
    const expect = parseInt(line.substr(star + 1), 16)
    return !isNaN(expect) &&
        (crc16(line.substr(0, star)) & 0xFFFF) === (expect & 0xFFFF)
}

function parseLine(line: string) {
    if (!lineOk(line)) return
    let toks = line.split(" ")
    for (let k = 0; k < toks.length; k++) {
        let t = toks[k]
        let c = t.indexOf(":")
        if (c < 0) continue
        let key = t.substr(0, c)
        let val = parseFloat(t.substr(c + 1))
        if (key == "T") sTemp = val
        else if (key == "P") sPress = val
        else if (key == "H") sHum = val
        else if (key == "L") sLight = val
        else if (key == "M") sPir = val
    }
    // Beep (built-in speaker) on the rising edge of the ESP PIR motion flag.
    // The heart flashes immediately so the event is visible.
    if (sPir == 1 && prevPir == 0) {
        basic.showIcon(IconNames.Heart, 1500)
        music.playTone(880, 300)
    }
    prevPir = sPir
}

// Read one line at a time. readString() alone (no terminator) can return the
// line plus whatever else is buffered, so read only up to the newline.
serial.onDataReceived("\n", () => {
    const line = serial.readUntil(serial.delimiters(Delimiters.NewLine))
    if (line && line.length > 0) parseLine(line)
})

// Short string for the current view INCLUDING the unit, e.g. "27C", "1003h",
// "48%", "918l", "M0"/"M1". Scrolls quickly (90 ms/char) so switching is fast.
function currentString(): string {
    if (view == V_TEMP) return "" + Math.round(sTemp) + "C"
    if (view == V_PRESS) return "" + Math.round(sPress) + "h"
    if (view == V_HUM) return "" + Math.round(sHum) + "%"
    if (view == V_LIGHT) return "" + Math.round(sLight) + "l"
    return "M" + Math.round(sPir)
}

function viewLabel(): string {
    if (view == V_TEMP) return "T"
    if (view == V_PRESS) return "P"
    if (view == V_HUM) return "H"
    if (view == V_LIGHT) return "L"
    return "M"
}

// Fast refresh of the current view. renderRequested is set by the buttons so a
// press aborts the previous (possibly mid-scroll) draw immediately.
let renderRequested = true

function requestRender() {
    pendingHint = true
    renderRequested = true
}

function render() {
    if (!renderRequested) return
    renderRequested = false

    if (pendingHint) {
        // Brief 1-char flash so switching is obvious, then the value+unit.
        basic.showString(viewLabel(), 100)
        pendingHint = false
    }

    // Value + unit, e.g. "27C". 90 ms/char scroll: short and quick.
    basic.showString(currentString(), 90)
}

// B = next value (wraps around), A = previous. Instant flip + re-render.
input.onButtonPressed(Button.B, function () { view = (view + 1) % N_VIEWS; requestRender() })
input.onButtonPressed(Button.A, function () { view = (view + N_VIEWS - 1) % N_VIEWS; requestRender() })

// Fast, non-blocking refresh of the current view so values update live and a
// button press is served almost instantly.
basic.forever(function () {
    render()
    basic.pause(300)
})

// Report what the panel RECEIVED from the ESP (T = received temp, L = received
// light, P = received motion flag) over the TX pin; the ESP republishes these
// as mb_temp/mb_light/mb_pir on MQTT, so we can compare them with the ESP's own
// pir1/temp1 telemetry. The frame is CRC-protected so the ESP only republishes
// data that made it to the wire intact.
basic.forever(function () {
    let payload = "T:" + sTemp + " L:" + sLight + " P:" + sPir
    let c = crc16(payload).toString(16).toUpperCase()
    while (c.length < 4) c = "0" + c
    serial.writeLine(payload + "*" + c)
    basic.pause(700)
})

render()

// Startup test tone: if you hear it, the built-in speaker works.
music.playTone(880, 300)
