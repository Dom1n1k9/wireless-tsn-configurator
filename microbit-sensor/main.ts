// WTSN micro:bit V2 BLE display panel (MakeCode / TypeScript)
// micro:bit = BLE peripheral. ESP32 BLE-central sends:  "T:22.1 H:55 L:120 P:0\n"
// Button A/B switch sensor; motion P=1 -> speaker beeps.

bluetooth.startUartService()

let temp = NaN
let hum = NaN
let lux = NaN
let motion = 0
let idx = 0

// Read incoming UART lines in a background loop (avoids the event-arg quirk).
basic.forever(function () {
    const line = bluetooth.uartReadUntil("\n")
    if (line) {
        const parts: string[] = ("" + line).split(" ")
        for (const p of parts) {
            const kv = p.split(":")
            if (kv.length === 2) {
                const k = kv[0]
                if (k === "T") temp = parseFloat(kv[1])
                else if (k === "H") hum = parseFloat(kv[1])
                else if (k === "L") lux = parseFloat(kv[1])
                else if (k === "P") motion = parseInt(kv[1])
            }
        }
        if (motion === 1) { music.playTone(880, 150); motion = 0 }
        render()
    }
})

function render() {
    if (idx === 2 && !isNaN(lux)) basic.showString("L" + Math.floor(lux))
    else if (idx === 1 && !isNaN(hum)) basic.showString("H" + Math.floor(hum))
    else if (idx === 0 && !isNaN(temp)) basic.showString("T" + Math.round(temp))
    else basic.showString(".")
}

input.onButtonPressed(Button.A, function () { idx = (idx + 1) % 3; render() })
input.onButtonPressed(Button.B, function () { idx = (idx + 2) % 3; render() })
