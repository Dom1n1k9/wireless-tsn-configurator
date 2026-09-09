// WTSN micro:bit V2 - wireless sensor node add-on (sound + onboard sensors)
// Sends its OWN sensors over UART to the ESP, which publishes them as mb_*:
//   "T:<temp> L:<light> P:0 N:<sound>"   (T/L are micro:bit's own sensors)
//   - T = input.temperature()  [C]
//   - L = input.lightLevel()   [0-255]
//   - N = input.soundLevel()   [0-255] (microphone)
//   - P = reserved (motion/actor)
// Clap/loud sound (N > 60) sends "C:identify" so the ESP reacts (buzzer beep).
// Buttons: A=identify, B=actor, A+B=reboot (commands to ESP via UART).
// Wiring: P0 TX --> ESP GPIO14 (RX); P1 RX <-- ESP GPIO15 (TX); GND --> GND
serial.redirect(SerialPin.P0, SerialPin.P1, BaudRate.BaudRate115200)

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
const HEXD = "0123456789ABCDEF"
function toHex4(n: number): string {
    let s = ""
    for (let i = 3; i >= 0; i--) {
        s = s + HEXD.charAt((n >> (i * 4)) & 0xF)
    }
    return s
}
function sendLine(payload: string) {
    serial.writeLine(payload + "*" + toHex4(crc16(payload)))
}

let sound = 0
let clapLatch = 0
basic.forever(function () {
    sound = input.soundLevel()
    // clap detect -> tell ESP (buzzer beep)
    if (sound > 60 && input.runningTime() - clapLatch > 800) {
        clapLatch = input.runningTime()
        sendLine("C:identify")
    }
    // send own sensors: temp, light, motion=0, sound
    sendLine("T:" + input.temperature() + " L:" + input.lightLevel() + " P:0 N:" + sound)
    basic.pause(250)
})

// buttons -> commands
input.onButtonPressed(Button.A, function () { sendLine("C:identify") })
input.onButtonPressed(Button.B, function () { sendLine("C:actor") })
input.onButtonPressed(Button.AB, function () { sendLine("C:reboot") })
