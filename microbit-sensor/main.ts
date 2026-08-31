// WTSN micro:bit V2 display panel - wired UART (NOT BLE)
// micro:bit = display + button input; sends sensor lines over serial pins to the ESP32
// agent, which republishes them on MQTT. Buttons switch the displayed value; motion
// (on P2) beeps. No bluetooth at all.
//
// Wiring:  micro:bit P0 (TX) --> ESP GPIO14 (UART RX), GND-->GND
//          optional P2 (PIR/hold) --> beep on motion

// Route serial to the physical pins (P0 TX / P1 RX) instead of USB.
serial.redirect(SerialPin.P0, SerialPin.P1, BaudRate.BaudRate115200)

let idx = 0
let counter = 0

function sendLine() {
    // "T:xx H:xx L:xx P:x" style line that the ESP parses.
    // Here simulated; connect your real sensor to P2 to feed P.
    let motion = pins.digitalReadPin(DigitalPin.P2)
    serial.writeLine("C:" + counter + " I:" + idx + " P:" + motion)
    counter = counter + 1
}

// send periodically
basic.forever(function () {
    sendLine()
    basic.pause(700)
    render()
})

function render() {
    if (idx === 0) basic.showString("" + counter)
    else if (idx === 1) basic.showString("I" + idx)
    else basic.showIcon(IconNames.Heart)
}

input.onButtonPressed(Button.A, function () { idx = (idx + 1) % 3; render() })
input.onButtonPressed(Button.B, function () { idx = (idx + 2) % 3; render() })
