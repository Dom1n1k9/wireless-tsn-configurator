// WTSN micro:bit V2 display panel - wired UART (NOT BLE)
//
// RX (P1): the ESP32 agent pushes its sensor values once a second,
//      "T:21.5 P:1013.2 H:48 L:1234 M:0 A:0"
// The panel shows ONE value with its unit and STAYS on it - no auto-cycling,
// no auto-refresh:
//      T:27C -> P:1006hPa -> H:48% -> L:918lx -> M:1 -> (back to T)
// B = next value, A = previous value. Nothing moves until you press a button.
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
let sActor = 0
let prevPir = 0

// Display views, in B/A cycle order: temp -> press -> hum -> light -> motion.
const V_TEMP = 0
const V_PRESS = 1
const V_HUM = 2
const V_LIGHT = 3
const V_MOTION = 4
const N_VIEWS = 5
let view = V_TEMP

// Parse "T:21.5 P:1013.2 H:48 L:1234 M:0 A:0" into the s* variables.
function parseLine(line: string) {
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
        else if (key == "A") sActor = val
    }
    // Beep (built-in speaker) on the rising edge of the ESP PIR motion flag.
    // The heart flashes immediately so the event is visible.
    if (sPir == 1 && prevPir == 0) {
        basic.showIcon(IconNames.Heart, 1500)
        music.playTone(880, 300)
    }
    prevPir = sPir
}

serial.onDataReceived("\n", () => {
    parseLine(serial.readString())
})

// Show the current value with its unit, e.g. "T:27C". The display keeps this
// until the next button press (or a motion flash).
function render() {
    if (view == V_TEMP) basic.showString("T:" + Math.round(sTemp) + "C")
    else if (view == V_PRESS) basic.showString("P:" + Math.round(sPress) + "hPa")
    else if (view == V_HUM) basic.showString("H:" + Math.round(sHum) + "%")
    else if (view == V_LIGHT) basic.showString("L:" + Math.round(sLight) + "lx")
    else basic.showString("M:" + Math.round(sPir))
}

// B = next value (wraps around), A = previous. Only way the view changes.
input.onButtonPressed(Button.B, function () { view = (view + 1) % N_VIEWS; render() })
input.onButtonPressed(Button.A, function () { view = (view + N_VIEWS - 1) % N_VIEWS; render() })

// Re-show the CURRENT view every 5 s: keeps the display alive and updates the
// value live, but never switches to another value.
basic.forever(function () {
    render()
    basic.pause(5000)
})

// DIAGNOSTIC (temporary): report what the panel RECEIVED from the ESP
// (T = received temp, L = received light, P = received motion flag) over the
// TX pin; the ESP republishes these as mb_temp/mb_light/mb_pir on MQTT, so we
// can compare them with the ESP's own pir1/temp1 telemetry.
basic.forever(function () {
    serial.writeLine("T:" + sTemp + " L:" + sLight + " P:" + sPir)
    basic.pause(700)
})

render()

// Startup test tone: if you hear it, the built-in speaker works.
music.playTone(880, 300)
