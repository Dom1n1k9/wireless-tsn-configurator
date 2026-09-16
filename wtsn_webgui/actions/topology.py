"""Architecture / topology data for the Node-RED-style flow diagram.

Builds a data-driven graph of every component in the W-TSN setup from the
current DB state (works in both Simulation and Real mode):

* core nodes: CNC (configurator), MQTT broker, access point / radio medium,
  the ESP32 agent boards, the ESP32-CAM and generic host nodes;
* sensor/actor components wired to each ESP with their GPIO / bus pin and the
  supply voltage taken from the actual firmware pin map;
* edges: FXMQTT / MQTT control plane (broker relay), OPC UA FX over MQTT C2C
  field exchange, 802.1Qcc TSN streams (through the radio / AP), gPTP 802.1AS,
  and local bus (I2C / ADC / GPIO / UART) wiring.

The front-end consumes this JSON and renders it as an animated flow diagram.
"""

import re

# ---------------------------------------------------------------------------
# Firmware pin map (must mirror esp32-agent/components/wtsn_agent/wtsn_sensor.c
# and esp32-cam/main/camera_app.c). Used to label each component's wiring.
# ---------------------------------------------------------------------------

SENSOR_BOARD = {
    "bme280": {
        "label": "BME280 sensor",
        "sub": "I2C SDA=GPIO21, SCL=GPIO22 @100kHz",
        "power": "3.3V",
        "bus": "I2C",
        "sensor_ids": ("temp1", "press1", "hum1"),
        "icons": {"temp1": "°C", "press1": "hPa", "hum1": "%"},
    },
    "imu": {
        "label": "IMU",
        "sub": "I2C (add-on)",
        "power": "3.3V",
        "bus": "I2C",
        "sensor_ids": ("imu1",),
        "icons": {"imu1": "g"},
    },
    "light": {
        "label": "Light (TEMT6000)",
        "sub": "ADC1_CH5 = GPIO33",
        "power": "3.3V",
        "bus": "ADC",
        "sensor_ids": ("light1",),
        "icons": {"light1": "lx"},
    },
    "pir": {
        "label": "PIR motion (HC-SR501)",
        "sub": "GPIO27 (digital in)",
        "power": "5V",
        "bus": "GPIO",
        "sensor_ids": ("pir1",),
        "icons": {"pir1": ""},
    },
    "buzzer": {
        "label": "Piezo buzzer",
        "sub": "GPIO25 (PWM)",
        "power": "3.3V",
        "bus": "GPIO",
        "sensor_ids": (),
        "icons": {},
    },
    "relay": {
        "label": "Actor relay / switch",
        "sub": "GPIO26 (out)",
        "power": "5V",
        "bus": "GPIO",
        "sensor_ids": ("actor_mode",),
        "icons": {"actor_mode": ""},
    },
    "wifi_motion": {
        "label": "WiFi Vision",
        "sub": "RSSI jitter / CSI (radio sensing)",
        "power": "3.3V",
        "bus": "RF",
        "sensor_ids": ("wifi_motion", "wifi_rssi"),
        "icons": {"wifi_motion": "", "wifi_rssi": "dBm"},
    },
    "microbit": {
        "label": "micro:bit V2",
        "sub": "UART GPIO14<RX, GPIO15>TX @115200",
        "power": "3V",
        "bus": "UART",
        "sensor_ids": ("mb_temp", "mb_light", "mb_sound", "mb_pir",
                       "mb_sync_offset", "mb_sync_jitter", "mb_sync_state"),
        "icons": {"mb_temp": "°C", "mb_light": "", "mb_sound": ""},
    },
    "gpio": {
        "label": "GPIO input",
        "sub": "digital I/O",
        "power": "3.3V",
        "bus": "GPIO",
        "sensor_ids": ("gpio1",),
        "icons": {"gpio1": ""},
    },
    "sonar": {
        "label": "Sonar HC-SR04 + servo",
        "sub": "TRIG=GPIO13, ECHO=GPIO12 · SG90 servo=GPIO18",
        "power": "5V",
        "bus": "GPIO",
        "sensor_ids": (),
        "icons": {},
    },
}

CAM_PINS = \
    "OV2640 senzor: XCLK=0, PWDN=32, SSCB=26/27, D0-D7=5,18,19,21,36,39,34,35, " \
    "VSYNC=25, HREF=23, PCLK=22 · SD: CS=13, SCK=14, MOSI=15, MISO=2 · RGB LED=33"

CAM_COMPONENT = {
    "label": "ESP32-CAM (AI-Thinker)",
    "sub": CAM_PINS,
    "power": "5V",
    "bus": "DVP",
}

# 802.1P -> WMM / 802.11e Access Category (per 802.11-2016 Table 9-2; the radio
# layer maps wired TSN priorities onto these queues since plain 802.11 cannot
# give hard TSN guarantees).
WMM = [
    ("AC_BK", "Background", (1, 2)),
    ("AC_BE", "Best effort", (0, 3)),
    ("AC_VI", "Video", (4, 5)),
    ("AC_VO", "Voice", (6, 7)),
]


def _wmm_for(prio):
    try:
        p = int(prio)
    except (TypeError, ValueError):
        p = 3
    for ac, name, prios in WMM:
        if p in prios:
            return "%s (%s)" % (ac, name)
    return "AC_BE (Best effort)"


def _dev_kind(dev):
    """Classify a device row: 'cam', 'esp' (sensor board) or 'host'."""
    kind = dev.get("kind")
    if kind == 5 or re.search(r"cam", dev.get("id", ""), re.I):
        return "cam"
    if str(dev.get("id", "")).startswith("esp32"):
        return "esp"
    return "host"


# Per-board wiring of this project's actual setup:
#   esp32-01 = sensor add-on board    → BME280 + light + PIR + buzzer (PIR alarm)
#                                      + WiFi Vision; NO relay/switch, NO micro:bit
#   esp32-02 = display/sync board     → micro:bit panel + piezo buzzer only;
#                                      NO sensors, NO relay/switch
# Any other ESP keeps the full component set.
BOARD_WIRING = {
    "esp32-01": ("bme280", "light", "pir", "buzzer", "wifi_motion"),
    "esp32-02": ("buzzer", "microbit", "sonar"),
}

_FULL_ESP_ORDER = ("bme280", "imu", "light", "pir", "buzzer", "relay",
                   "wifi_motion", "microbit", "gpio")


def _esp_components(con, dev):
    """Return the list of sensor/actor components wired to an ESP agent."""
    did = dev["id"]
    rows = con.execute("SELECT sensor_id,value,unit,last_update FROM sensors "
                       "WHERE device_id=?", (did,)).fetchall()
    present = {r["sensor_id"]: dict(r) for r in rows}
    comps = []
    order = BOARD_WIRING.get(did, _FULL_ESP_ORDER)
    for key in order:
        spec = SENSOR_BOARD[key]
        if not spec["sensor_ids"]:
            # always-present board peripherals (buzzer) whose presence is not
            # tied to a reported telemetry sensor
            comps.append({"id": "%s:%s" % (did, key), "key": key, **spec,
                          "values": {}})
            continue
        hits = [s for s in spec["sensor_ids"] if s in present]
        if not hits:
            continue
        values = {s: present[s] for s in hits}
        comps.append({"id": "%s:%s" % (did, key), "key": key, **spec,
                      "values": values, "_present": True})
    return comps


def _cam_components(con, dev):
    return [{"id": "%s:camera" % dev["id"], "key": "camera", **CAM_COMPONENT,
             "values": {}}]


def _topology(con, body):
    mode = body.get("mode") or ""

    from .. import state
    with state.BROKER_LOCK:
        brok_ok = state.BROKER.get("ok", False) if state.MODE.get("mode") == "real" else True

    brk = con.execute("SELECT value FROM settings WHERE key='broker'").fetchone()
    broker = brk["value"] if brk and brk["value"] else "127.0.0.1:1883"
    srv = con.execute("SELECT value FROM settings WHERE key='server_type'").fetchone()
    server_type = (srv["value"] if srv and srv["value"] else "pc")
    gm_rows = con.execute("SELECT grandmaster FROM timesync_status "
                          "WHERE id='main'").fetchone()
    gm = (gm_rows["grandmaster"] if gm_rows and gm_rows["grandmaster"] else "PC")

    devices = [dict(r) for r in con.execute("SELECT * FROM devices "
                                            "ORDER BY id")]
    sensors = con.execute("SELECT device_id,sensor_id,value,unit,last_update "
                          "FROM sensors").fetchall()

    nodes = []
    edges = []
    flows = []

    # ---- core infrastructure ------------------------------------------
    nodes.append({"id": "pc", "type": "cnc", "label": "PC / CNC",
                  "sub": "configurator + web GUI", "power": "-",
                  "status": "online"})
    nodes.append({"id": "broker", "type": "broker", "label": "MQTT broker",
                  "sub": broker, "power": "-", "status": "online"})
    nodes.append({"id": "ap", "type": "radio", "label": "WiFi AP (~2.4/5 GHz)",
                  "sub": "802.11 radio medium · WMM queues",
                  "power": "-", "status": "online"})

    edges.append({"from": "pc", "to": "broker", "type": "mqtt",
                  "label": "FXMQTT / OPC UA FX (management)",
                  "topics": ["tsn/cmd/<id>/apply", "tsn/cmd/<id>/ping",
                             "tsn/cmd/<id>/ota"]})

    # control plane: broker -> each device
    for d in devices:
        did = d["id"]
        edges.append({"from": "broker", "to": did, "type": "mqtt",
                      "label": "commands apply / ping / OTA",
                      "topics": ["tsn/cmd/%s/*" % did]})
        edges.append({"from": did, "to": "broker", "type": "mqtt",
                      "label": "status / ack / sensors / ptp",
                      "topics": ["tsn/ack/%s" % did, "tsn/status",
                                 "tsn/sensors", "tsn/ptp"]})

    # FX C2C field exchange (server -> clients via broker)
    fx_nodes = [d["id"] for d in devices]
    if server_type == "node":
        fid = None
        for r in con.execute("SELECT value FROM settings "
                             "WHERE key='server_id'").fetchall():
            fid = r["value"]
        if fid and fid in fx_nodes:
            fx_nodes.remove(fid)
            for cli in fx_nodes:
                edges.append({"from": "broker", "to": cli, "type": "fx",
                              "label": "FX C2C field exchange (participant)",
                              "topics": ["tsn/fx/data", "tsn/fx/%s" % cli]})
    else:
        for cli in fx_nodes:
            edges.append({"from": "broker", "to": cli, "type": "fx",
                          "label": "FX C2C field exchange",
                          "topics": ["tsn/fx/data", "tsn/fx/%s" % cli]})

    # ---- TSN streams (talker -> AP -> listeners) ----------------------
    dev_ids = {d["id"] for d in devices}
    streams = con.execute("SELECT * FROM tsn_streams").fetchall()
    for srow in streams:
        s = dict(srow)
        memb = {r["role"]: r["device_id"] for r in con.execute(
            "SELECT role,device_id FROM tsn_stream_members "
            "WHERE stream_id=?", (s["stream_id"],)).fetchall()}
        talker = memb.get("talker") or s["talker"] or ""
        listeners = [v for k, v in memb.items() if k == "listener"]
        prio = None
        if "priority" in s.keys():
            prio = s["priority"]
        q = _wmm_for(prio)
        lbl = "802.1Qcc stream '%s' · VLAN %s · WMM %s" % (
            s.get("name") or s["stream_id"], s.get("vlan_id", "-"), q)
        if talker:
            edges.append({"from": talker, "to": "ap", "type": "tsn-stream",
                          "label": "TSN talker · %s" % q,
                          "stream": s["stream_id"],
                          "topics": ["tsn/fx/cmd/%s" % talker]})
        else:
            edges.append({"from": "pc", "to": "ap", "type": "tsn-stream",
                          "label": lbl, "stream": s["stream_id"], "topics": []})
        for lst in listeners:
            edges.append({"from": "ap", "to": lst, "type": "tsn-stream",
                          "label": "TSN listener · %s" % q,
                          "stream": s["stream_id"],
                          "topics": ["tsn/fx/cmd/%s" % lst]})

    # data plane: every device connects through the AP (radio medium)
    for did in dev_ids:
        edges.append({"from": "ap", "to": did, "type": "radio",
                      "label": "802.11 data plane · WMM",
                      "topics": []})

    # ---- gPTP 802.1AS (grandmaster -> AP -> slaves) --------------------
    slaves = set()
    sync_settings = con.execute("SELECT value FROM settings "
                                "WHERE key='sync_nodes'").fetchone()
    if sync_settings and sync_settings["value"]:
        slaves = set(x for x in sync_settings["value"].split(",") if x)
    gm_is_real = gm in dev_ids
    if gm == "PC":
        src = "pc"
    elif gm_is_real:
        src = gm
    else:
        src = "pc"
    edges.append({"from": src, "to": "ap", "type": "ptp",
                  "label": "gPTP 802.1AS · grandmaster",
                  "topics": ["tsn/ptp"]})
    for sl in (slaves - {src}):
        edges.append({"from": "ap", "to": sl, "type": "ptp",
                      "label": "gPTP slave (offset/jitter)",
                      "topics": ["tsn/ptp"]})

    # ---- devices + local sensors ---------------------------------------
    for d in devices:
        did = d["id"]
        kind = _dev_kind(d)
        features = [r["feature"] for r in con.execute(
            "SELECT feature FROM device_tsn_features WHERE device_id=?",
            (did,)).fetchall()]
        ip = d.get("ip") or ""
        sub = kind
        if ip:
            sub += " · " + ip
        if d.get("firmware"):
            sub += " · " + d["firmware"]
        node = {"id": did, "type": kind,
                "label": d.get("name") or did,
                "sub": sub,
                "power": "5V in / 3.3V logic" if kind in ("esp", "cam") else "-",
                "status": "online" if d.get("status") == 0 else "offline",
                "tsn": features,
                "ip": ip}
        nodes.append(node)
        # wx firmware kind: 0=esp32, 5=cam
        if kind == "esp":
            for c in _esp_components(con, d):
                cnode = {"id": c["id"], "type": "sensor", "key": c["key"],
                         "label": c["label"], "sub": c["sub"],
                         "power": c["power"], "bus": c["bus"],
                         "status": "online",
                         "parent": did,
                         "values": {k: {"value": v["value"], "unit": v["unit"]}
                                    for k, v in c.get("values", {}).items()}}
                nodes.append(cnode)
                edges.append({"from": c["id"], "to": did, "type": c["bus"].lower(),
                              "label": "%s · %s" % (c["bus"], c["power"]),
                              "topics": []})
        elif kind == "cam":
            for c in _cam_components(con, d):
                cnode = {"id": c["id"], "type": "sensor", "key": "camera",
                         "label": c["label"], "sub": c["sub"],
                         "power": c["power"], "bus": "DVP",
                         "status": "online", "parent": did,
                         "values": {}}
                nodes.append(cnode)
                edges.append({"from": c["id"], "to": did, "type": "dvp",
                              "label": "DVP parallel · %s" % c["power"],
                              "topics": []})
        else:
            # generic host: attach any direct sensors it reports
            for r in sensors:
                if r["device_id"] != did:
                    continue
                cid = "%s:raw.%s" % (did, r["sensor_id"])
                nodes.append({"id": cid, "type": "sensor", "key": "raw",
                              "label": r["sensor_id"], "sub": "reported sensor",
                              "power": "-", "bus": "-", "status": "online",
                              "parent": did,
                              "values": {r["sensor_id"]: {"value": r["value"],
                                                        "unit": r["unit"]}}})
                edges.append({"from": cid, "to": did, "type": "bus",
                              "label": "telemetry", "topics": []})
        # drop the broker<->device MQTT edge if no link (handled above)

    # deduplicate node list by id (camera component added once per cam)
    seen = set()
    nodes = [n for n in nodes if not (n["id"] in seen or seen.add(n["id"]))]

    # ---- human-readable flow narrative --------------------------------
    flows.append({"id": "ctrl", "title": "FXMQTT / OPC UA FX — control plane",
                  "steps": [
                      "CNC builds a per-device JSON snapshot (QoS, VLAN, TAS, gPTP, streams)",
                      "published on tsn/cmd/<id>/apply over MQTT",
                      "agent applies it on the ESP and ACKs on tsn/ack/<id>",
                      "telemetry (tsn/sensors, tsn/ptp) flows back to the GUI" ]})
    flows.append({"id": "fx", "title": "OPC UA FX / C2C field exchange",
                  "steps": [
                      "field server (PC or selected node) publishes on tsn/fx/data",
                      "participants exchange values via the broker",
                      "stream reservations go out on tsn/fx/cmd/<talker>" ]})
    flows.append({"id": "tsn", "title": "TSN data plane over WiFi",
                  "steps": [
                      "talker tags frames with VLAN ID + 802.1P priority",
                      "802.1P priority is mapped onto WMM/802.11e queue "
                      "(AC_VO/VI/BE/BK) by the radio layer",
                      "802.1Qbv TAS / GCL gates time-critical traffic",
                      "802.1Qcc reserved stream is delivered to the listener(s)",
                      "gPTP 802.1AS keeps device clocks synchronized (PC or ESP = GM)" ]})

    return {"ok": True, "mode": mode or "",
            "broker": broker, "broker_ok": brok_ok,
            "fx_server": server_type,
            "gm": gm,
            "wmm": [{"ac": ac, "name": name, "prios": list(p)}
                    for ac, name, p in WMM],
            "nodes": nodes, "edges": edges, "flows": flows,
            "sensors": [dict(r) for r in sensors]}


HANDLERS = {"topology": _topology}
