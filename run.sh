#!/usr/bin/env bash
set -e

# ============================================================
#  WTSN Configurator - one launcher for everything
#  MQTT broker + web GUI + browser (+ optional flash)
# ============================================================

PROJ_DIR="$HOME/Documents/wtsn-configurator"
ESP_DIR="$PROJ_DIR/esp32-agent"
IDF_PATH="$HOME/esp/eim_workspace/v5.3/esp-idf"
MQTT_HOST_FALLBACK="192.168.0.149"   # this PC IP (broker) - auto-detected below
MQTT_PORT=1883
GUI_PORT=8000

# detect current LAN IP (first non-loopback)
LAN_IP=$(ip -4 addr show 2>/dev/null | grep -oE "inet [0-9.]+" | grep -v "127.0.0.1" | head -1 | sed 's/inet //')
[ -z "$LAN_IP" ] && LAN_IP="$MQTT_HOST_FALLBACK"

log() { echo -e "\033[1;36m[wtsn]\033[0m $*"; }

# ---------------- 1) MQTT broker ----------------
ensure_broker() {
    # make sure mosquitto listens on 0.0.0.0 (reachable from ESP32).
    # NOTE: this needs sudo. If you are not root, run these once manually:
    #   sudo tee /etc/mosquitto/conf.d/wtsn.conf <<EOF
    #   listener 1883 0.0.0.0
    #   allow_anonymous true
    #   EOF
    #   sudo systemctl restart mosquitto
    if ! grep -q "listener $MQTT_PORT 0.0.0.0" /etc/mosquitto/conf.d/wtsn.conf 2>/dev/null; then
        log "broker config missing -> needs sudo. Run these manually:"
        log "  echo 'listener 1883 0.0.0.0' | sudo tee /etc/mosquitto/conf.d/wtsn.conf"
        log "  echo 'allow_anonymous true' | sudo tee -a /etc/mosquitto/conf.d/wtsn.conf"
        log "  sudo systemctl restart mosquitto"
        exit 1
    fi

    if pgrep -x mosquitto >/dev/null 2>&1; then
        log "broker already running"
    else
        # run as the current user (needs port > 1024 config already in place)
        if [ "$(id -u)" = "0" ]; then
            mosquitto -c /etc/mosquitto/mosquitto.conf -d
        else
            log "starting mosquitto (may ask for sudo password if it aborts)..."
            mosquitto -c /etc/mosquitto/mosquitto.conf -d 2>/dev/null || \
                (echo "sudo prompt needed"; sudo -n mosquitto -c /etc/mosquitto/mosquitto.conf -d 2>/dev/null || \
                 log "could not auto-start broker - start it manually")
        fi
        sleep 2
    fi

    if ss -tlnp 2>/dev/null | grep -q ":$MQTT_PORT "; then
        log "MQTT broker OK on 0.0.0.0:$MQTT_PORT (ESP32 connects to $LAN_IP:$MQTT_PORT)"
    else
        log "WARNING: broker not listening on $MQTT_PORT"
    fi
}

# ---------------- 2) web GUI ----------------
ensure_gui() {
    if ss -tln 2>/dev/null | grep -q ":$GUI_PORT "; then
        log "web GUI already running on http://127.0.0.1:$GUI_PORT"
        return
    fi
    log "starting web GUI (broker $LAN_IP:$MQTT_PORT) ..."
    cd "$PROJ_DIR"
    nohup python3 webgui.py --mqtt-host "$LAN_IP" --mqtt-port "$MQTT_PORT" \
         > /tmp/webgui.log 2>&1 & disown
    sleep 3
    if ss -tln 2>/dev/null | grep -q ":$GUI_PORT "; then
        log "web GUI OK on http://127.0.0.1:$GUI_PORT"
    else
        log "ERROR: web GUI failed to start - see /tmp/webgui.log"
    fi
}

# ---------------- 3) browser ----------------
open_browser() { xdg-open "http://127.0.0.1:$GUI_PORT" >/dev/null 2>&1 & }

# ---------------- 4) provisioning helper in a new terminal ----------------
PROV_SCRIPT="/tmp/wtsn_prov_helper.sh"
write_prov_script() {
    MASTER_IP="$LAN_IP"
    cat > "$PROV_SCRIPT" <<EOF
#!/usr/bin/env bash
sleep 1
echo '============================================'
echo ' WTSN ESP32 WiFi provisioning'
echo '============================================'
echo
echo '1. Connect this machine to the ESP SoftAP:'
echo '   SSID:   WTSN-Setup'
echo '   (no password)'
echo
echo '2. Then open in the browser:'
echo '   http://192.168.4.1/'
echo
echo '3. Enter your WiFi SSID/password and MQTT broker:'
echo "   ${MASTER_IP}  (this PC)"
echo
echo '4. Save - ESP32 reboots, blinks LED 3x and joins your WiFi,'
echo '   then announces itself on MQTT.'
echo
echo 'TIP: many routers isolate WiFi client multicast, so for PTP'
echo '  between two ESP32 nodes use an AP without client isolation.'
exec bash
EOF
    chmod +x "$PROV_SCRIPT"
}
open_prov_terminal() {
    write_prov_script
    local term=""
    for t in gnome-terminal konsole xfce4-terminal x-terminal-emulator; do
        if command -v "$t" >/dev/null 2>&1; then term="$t"; break; fi
    done
    if [ -n "$term" ]; then
        log "opening provisioning helper in a new terminal ($term)"
        case "$term" in
            gnome-terminal)   nohup gnome-terminal -- bash "$PROV_SCRIPT" >/dev/null 2>&1 & ;;
            konsole)         nohup konsole -e bash "$PROV_SCRIPT" >/dev/null 2>&1 & ;;
            xfce4-terminal)  nohup xfce4-terminal -e bash "$PROV_SCRIPT" >/dev/null 2>&1 & ;;
            x-terminal-emulator) nohup "$term" -e bash "$PROV_SCRIPT" >/dev/null 2>&1 & ;;
        esac
    else
        log "no graphical terminal found; run provisioning manually:"
        log "   iwctl station wlan0 connect WTSN-Setup   (or NetworkManager GUI)"
        log "   then open http://192.168.4.1/"
    fi
}

# ---------------- 5) optional flash ----------------
do_flash() {
    if [ ! -f "$ESP_DIR/build/wtsn_esp32_agent.bin" ]; then
        log "firmware not built - building ..."
        bash -c "source $IDF_PATH/export.sh >/dev/null 2>&1 && cd $ESP_DIR && idf.py build"
    fi
    PORT_UART=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1)
    [ -z "$PORT_UART" ] && PORT_UART="/dev/ttyUSB0"
    log "flashing to $PORT_UART ..."
    bash -c "source $IDF_PATH/export.sh >/dev/null 2>&1 && cd $ESP_DIR && idf.py -p $PORT_UART flash"
    log "firmware flashed. ESP32 restarts."
    log "If WiFi is NOT stored in NVS, it starts the WTSN-Setup SoftAP provisioning portal."
    xdg-open "http://192.168.4.1/" >/dev/null 2>&1 &
    log "Connect to WiFi 'WTSN-Setup' and open http://192.168.4.1/ to enter your WiFi details."
}

main() {
    ensure_broker
    ensure_gui
    open_browser
    open_prov_terminal
    log "Done. Broker=$LAN_IP:$MQTT_PORT  GUI=http://127.0.0.1:$GUI_PORT"
    log "ESP32: switch web GUI to REAL, add device id=esp32-01, then Deploy."
    log "New terminal opened with WiFi provisioning helper for the ESP32."
    if [ "$1" = "--flash" ]; then do_flash; fi
}

main "$@"