"""Simulation engine: fabricates devices, sensors and a realistic frame flow."""
import copy
import json
import random
import threading
import time

from . import state
from .db import add_event, connect

TSN_FUNCS = ["802.1Q QoS", "802.1Q VLAN", "gPTP 802.1AS", "802.1Qbv TAS",
             "802.1Qbu Preemption", "OPC UA", "OPC UA PubSub", "FX Multicast"]

# The simulated fleet is a fixed, explicit set. It is deterministic (no random
# selection) so a webgui restart always shows the same network and every device
# referenced by demo configuration (streams, VLANs, time sync) always exists.
# The IDs are also stable across ticks so sensors/status drift in place. Note
# there is deliberately no simulated "rpi" node: the Raspberry Pi is the CNC /
# edge host itself, not a TSN endpoint, and a fake one would shadow it.
_QOS, _VLAN, _GPTP, _TAS, _PRE, _OpcUa, _PubSub, _FX = TSN_FUNCS
_SIM_FLEET = [
    # kind 0 = TSN endpoint (sensor/gateway), 3 = STM32, 5 = camera
    {"id": "esp32-01", "name": "ESP32 Sensor", "ip": "192.168.1.10",
     "mac": "AA:BB:CC:00:01", "kind": 0, "firmware": "2.0.0", "rssi": -55,
     "usb": "ttyUSB0", "tsn": [_QOS, _VLAN, _GPTP, _TAS, _PRE, _OpcUa]},
    {"id": "esp32-02", "name": "ESP32 Sonar", "ip": "192.168.1.11",
     "mac": "AA:BB:CC:00:02", "kind": 0, "firmware": "2.0.0", "rssi": -61,
     "usb": "ttyUSB1", "tsn": [_QOS, _VLAN, _GPTP, _TAS, _FX]},
    {"id": "esp32-cam", "name": "ESP32-CAM", "ip": "192.168.1.60",
     "mac": "AA:BB:CC:00:06", "kind": 5, "firmware": "2.0.0", "rssi": -58,
     "usb": "ttyACM0", "tsn": [_QOS, _VLAN, _OpcUa, _FX]},
    {"id": "esp32-03", "name": "ESP32 Gateway", "ip": "192.168.1.12",
     "mac": "AA:BB:CC:00:03", "kind": 0, "firmware": "1.8.3", "rssi": -63,
     "usb": "ttyUSB2", "tsn": [_QOS, _VLAN, _GPTP, _PubSub, _FX]},
    {"id": "stm32-01", "name": "STM32 Sensor", "ip": "192.168.1.40",
     "mac": "AA:BB:CC:00:04", "kind": 3, "firmware": "1.4.0", "rssi": -66,
     "usb": "ttyUSB3", "tsn": [_QOS, _GPTP, _TAS, _OpcUa]},
    {"id": "nxp-01", "name": "NXP Node", "ip": "192.168.1.50",
     "mac": "AA:BB:CC:00:05", "kind": 0, "firmware": "2.1.0", "rssi": -70,
     "usb": "ttyUSB4", "tsn": [_QOS, _VLAN, _TAS, _PRE, _PubSub]},
    {"id": "linux-01", "name": "Linux Node", "ip": "192.168.1.30",
     "mac": "AA:BB:CC:00:07", "kind": 0, "firmware": "3.0.1", "rssi": -52,
     "usb": "ttyUSB5", "tsn": [_QOS, _VLAN, _GPTP, _OpcUa, _PubSub]},
]

SIM_STABLE_DEVICES = None
SIM_STABLE_LOCK = threading.Lock()


def _gen_stable_devices():
    """Return the fixed simulated device set (deep-copied so per-tick mutations
    never leak into the template). Reused every tick so the device list stays
    constant while sensor values / status continue to drift.

    esp32-01 carries the sensor add-on board (BME280 + light + PIR +
    WiFiVision), esp32-02 the micro:bit display/sync board plus the panning
    sonar, esp32-cam the camera. The rest cover the supported endpoint kinds.
    """
    return copy.deepcopy(_SIM_FLEET)


def sim_tick():
    global SIM_STABLE_DEVICES
    con = connect()
    try:
        with state.SIM_USER_DEVICES_LOCK:
            keep = list(state.SIM_USER_DEVICES)
        kept = {}
        for k in keep:
            row = con.execute("SELECT * FROM devices WHERE id=?", (k,)).fetchone()
            if row:
                kept[k] = dict(row)
            else:
                with state.SIM_USER_DEVICES_LOCK:
                    state.SIM_USER_DEVICES.discard(k)
        saved_ts = con.execute("SELECT * FROM timesync_status WHERE id='main'").fetchone()
        saved_ts = dict(saved_ts) if saved_ts else None
        saved_nodes = con.execute("SELECT * FROM settings WHERE key='sync_nodes'").fetchone()
        saved_nodes = dict(saved_nodes) if saved_nodes else None
        # Config tables are user configuration and must NOT be regenerated/destroyed by
        # the simulator. Simulated runtime data (devices/sensors/timesync) is UPSERTed
        # each tick with a stable simulated device set (generated once) so devices do
        # not appear/disappear each tick AND user-owned rows survive. Only rows that
        # are no longer simulated and are not user-owned are removed.
        with SIM_STABLE_LOCK:
            if SIM_STABLE_DEVICES is None:
                SIM_STABLE_DEVICES = _gen_stable_devices()
            stable = list(SIM_STABLE_DEVICES)
        devs = [d["id"] for d in stable]
        sim_ids = set(devs)
        for row in con.execute("SELECT id FROM devices"):
            rid = row[0]
            if rid in sim_ids:
                continue
            with state.SIM_USER_DEVICES_LOCK:
                is_user = rid in state.SIM_USER_DEVICES
            if not is_user:
                con.execute("DELETE FROM devices WHERE id=?", (rid,))
                con.execute("DELETE FROM device_tsn_features WHERE device_id=?", (rid,))
                con.execute("DELETE FROM sensors WHERE device_id=?", (rid,))
        # Re-upsert devices each tick so they stay in the list. ON CONFLICT
        # DO UPDATE (not INSERT OR REPLACE) so columns managed elsewhere
        # (last_deploy_at/ok, heartbeat_at, ...) survive the tick.
        for k, kv in kept.items():
            con.execute("INSERT INTO devices(id,name,ip,mac,kind,firmware,status,"
                        "last_seen,domain,rssi,usb) VALUES(?,?,?,?,?,?,?,?,?,?,?)"
                        " ON CONFLICT(id) DO UPDATE SET name=excluded.name,"
                        "ip=excluded.ip,mac=excluded.mac,kind=excluded.kind,"
                        "firmware=excluded.firmware,status=excluded.status,"
                        "last_seen=excluded.last_seen,domain=excluded.domain,"
                        "rssi=excluded.rssi,usb=excluded.usb",
                        (k, kv.get("name", ""), kv.get("ip", ""), kv.get("mac", ""),
                         kv.get("kind", 0), kv.get("firmware", ""), kv.get("status", 0),
                         kv.get("last_seen", int(time.time())), kv.get("domain", "default"),
                         kv.get("rssi", 0), kv.get("usb", "") or ""))
        for sd in stable:
            did = sd["id"]
            con.execute("INSERT INTO devices(id,name,ip,mac,kind,firmware,status,"
                        "last_seen,domain,rssi,usb) VALUES(?,?,?,?,?,?,0,strftime('%s','now'),"
                        "'default',?,?)"
                        " ON CONFLICT(id) DO UPDATE SET name=excluded.name,"
                        "ip=excluded.ip,mac=excluded.mac,kind=excluded.kind,"
                        "firmware=excluded.firmware,status=0,last_seen=excluded.last_seen,"
                        "rssi=excluded.rssi,usb=excluded.usb",
                        (did, sd["name"], sd["ip"], sd["mac"], sd["kind"],
                         sd["firmware"], sd.get("rssi", 0), sd.get("usb", "") or ""))
            prev_feats = set(r[0] for r in con.execute(
                "SELECT feature FROM device_tsn_features WHERE device_id=?", (did,)))
            for f in sd["tsn"]:
                if f not in prev_feats:
                    con.execute("INSERT INTO device_tsn_features(device_id,feature) VALUES(?,?)",
                                (did, f))
        # Sensor values drift; append one history sample per sensor for sparklines.
        # ESP32 boards (kind 0) carry the full sensor add-on board so the Sensors
        # page and Architecture wiring diagram show every component in Simulation.
        for sd in stable:
            did = sd["id"]
            # Sensor board only exists on the ESP32 agent ("esp32-*"), matching
            # the Architecture diagram's device classification; other nodes get
            # the generic temp/press/imu/gpio telemetry.
            is_esp = str(did).startswith("esp32")
            # esp32-01 = sensor add-on board (BME280, light, PIR, WiFi Vision,
            # buzzer). esp32-02 = display/sync board with the micro:bit panel +
            # buzzer only (no relay/switch, no sensors). Mirrors BOARD_WIRING
            # in actions/topology.py so Simulation and Architecture agree.
            sensor_board = (("temp1", 0, "C", 25.0), ("press1", 1, "hPa", 1005.0),
                            ("hum1", 0, "%", 42.0), ("light1", 4, "lx", 300.0),
                            ("pir1", 4, "", 0.0), ("wifi_motion", 4, "", 0.0))
            microbit_board = (("mb_temp", 0, "C", 25.0), ("mb_light", 4, "lx", 200.0),
                              ("mb_pir", 4, "", 0.0), ("mb_sound", 4, "", 0.0),
                              ("dist1", 3, "cm", 120.0), ("sonar_angle", 4, "deg", 90.0))
            generic = (("temp1", 0, "C", 25.0), ("press1", 1, "hPa", 1005.0),
                       ("imu1", 2, "g", 0.3), ("gpio1", 4, "V", 1.0))
            ai_board = (("ai_detect", 4, "", 0.0), ("ai_person", 4, "", 0.0),
                        ("rssi", 0, "dBm", -58.0))
            if did == "esp32-01":
                board = sensor_board
            elif did == "esp32-02":
                board = microbit_board
            elif did == "esp32-cam":
                board = ai_board
            else:
                board = (sensor_board if is_esp else generic)
            ai_person = 0
            for sid, typ, unit, basev in board:
                val = round(basev + random.uniform(-1.5, 1.5), 1)
                if sid == "pir1":
                    val = random.choice([0, 0, 0, 1])
                if sid == "ai_person":
                    val = 1 if random.random() < 0.25 else 0
                    ai_person = val
                if sid == "ai_detect":
                    val = random.choice([1, 2, 3]) if ai_person else 0
                con.execute("INSERT OR REPLACE INTO sensors(device_id,sensor_id,type,name,"
                            "value,unit,healthy,last_update) VALUES(?,?,?,?,?,?,1,strftime('%s','now'))",
                            (did, sid, typ, sid, val, unit))
                con.execute("INSERT INTO sensor_history(device_id,sensor_id,ts,value) "
                            "VALUES(?,?,strftime('%s','now'),?)", (did, sid, val))
        con.execute("DELETE FROM sensor_history WHERE ts < strftime('%s','now','-1 hours')")
        # Simulated OPC UA FX C2C field exchange: participants publish a data
        # set (a few sensor values) on tsn/fx/data, which the FXMQTT page shows
        # live. Only the mapped sensors are exchanged, with some sparsity.
        if random.random() < 0.7:
            fx_map = {"temp1": "temp_c", "press1": "pressure_hpa",
                      "hum1": "humidity_pct", "light1": "light_lx",
                      "pir1": "motion", "wifi_motion": "wifi_motion",
                      "ai_person": "ai_person", "ai_detect": "ai_objects",
                      "dist1": "sonar_cm", "mb_temp": "temp_c",
                      "mb_sound": "sound_db"}
            n_fx = 0
            for row in con.execute("SELECT device_id,sensor_id,value FROM sensors"):
                data_id = fx_map.get(row["sensor_id"])
                if not data_id or random.random() < 0.5:
                    continue
                con.execute("INSERT INTO fx_data(ts,src,data_id,value)"
                            " VALUES(strftime('%s','now'),?,?,?)",
                            (row["device_id"], data_id, row["value"]))
                n_fx += 1
            if n_fx:
                con.execute("DELETE FROM fx_data WHERE id NOT IN"
                            " (SELECT id FROM fx_data ORDER BY id DESC LIMIT 500)")
                if random.random() < 0.15:
                    add_event("fx", "cnc", "tsn/fx/data <- %d values (C2C exchange)" % n_fx)
        # 802.1Qcc stream runtime state: mostly ready, but the reservation
        # occasionally wobbles (standby / failed) so the Streams page is alive.
        for sr in con.execute("SELECT stream_id,status FROM tsn_streams").fetchall():
            r = random.random()
            if sr["status"] == 1:
                if r < 0.02:
                    con.execute("UPDATE tsn_streams SET status=3 WHERE stream_id=?",
                                (sr["stream_id"],))
                    add_event("streams", "cnc",
                              "stream %s -> standby (listener silent)" % sr["stream_id"])
                elif r > 0.995:
                    con.execute("UPDATE tsn_streams SET status=2 WHERE stream_id=?",
                                (sr["stream_id"],))
                    add_event("streams", "cnc",
                              "stream %s -> failed (reservation timeout)" % sr["stream_id"])
            elif sr["status"] == 3 and r < 0.2:
                con.execute("UPDATE tsn_streams SET status=1 WHERE stream_id=?",
                            (sr["stream_id"],))
                add_event("streams", "cnc", "stream %s -> ready" % sr["stream_id"])
            elif sr["status"] == 2 and r < 0.1:
                con.execute("UPDATE tsn_streams SET status=1 WHERE stream_id=?",
                            (sr["stream_id"],))
                add_event("streams", "cnc",
                          "stream %s -> ready (re-reserved)" % sr["stream_id"])
        # Simulated sonar sweeps (esp32-02) and WiFiVision RSSI sweeps (esp32-01)
        # so the Sensors page can render the radar + WiFiVision maps even in
        # Simulation mode. A "sweep" drifts through 0..179° anti-clockwise.
        now_ss = int(time.time())
        for sd in stable:
            if sd["id"] == "esp32-02" and random.random() < 0.5:
                sweep = [round(random.uniform(20, 180), 1) for _ in range(36)]
                con.execute("INSERT INTO sonar_sweeps(device_id,ts,sweep_id,sweep) "
                            "VALUES(?,?,?,?)",
                            (sd["id"], now_ss, (now_ss % 100000),
                             json.dumps(sweep)))
                add_event("mqtt", sd["id"],
                          "sonar sweep: %d angles" % len(sweep))
            if sd["id"] == "esp32-01" and random.random() < 0.5:
                rippled = [round(max(0.0, 10.0 + random.gauss(0, 2.5)), 2)
                           for _ in range(36)]
                con.execute("INSERT INTO sonar_sweeps(device_id,ts,sweep_id,sweep) "
                            "VALUES(?,?,?,?)",
                            (sd["id"], now_ss, (now_ss % 100000),
                             json.dumps(rippled)))
        gm = devs[0] if devs else "esp32-01"
        if saved_ts and saved_ts["grandmaster"]:
            gm = saved_ts["grandmaster"]
            con.execute("INSERT OR REPLACE INTO timesync_status(id,mode,grandmaster,offset_ns,quality)"
                       " VALUES('main',?,?,?,?)", (saved_ts["mode"], saved_ts["grandmaster"],
                                                    saved_ts["offset_ns"], saved_ts["quality"]))
        else:
            con.execute("INSERT OR REPLACE INTO timesync_status(id,mode,grandmaster,offset_ns,quality)"
                        " VALUES('main',1,?,?,?)", (gm, random.randint(-50, 500),
                                                     random.randint(80, 99)))
        if saved_nodes and saved_nodes["value"]:
            con.execute("INSERT OR REPLACE INTO settings(key,value) VALUES('sync_nodes',?)",
                       (saved_nodes["value"],))
        # Simulated telemetry for the Metrics page (latency_log + timesync_reports).
        # One latency sample every ~4 ticks and one gPTP report per tick, so the
        # Metrics sparklines/summary have data to show in Simulation mode too.
        now_i = int(time.time())
        for sd in stable:
            if random.random() < 0.25:
                con.execute("INSERT INTO latency_log(device_id,ts,latency_ms) VALUES(?,?,?)",
                            (sd["id"], now_i, round(random.uniform(2, 40), 1)))
        if random.random() < 0.9:
            con.execute("INSERT INTO timesync_reports(device_id,ts,offset_ns,jitter_ns,"
                        "packet_count,packet_loss,status) VALUES(?,?,?,?,?,?,?)",
                        (devs[0] if devs else gm, now_i,
                         random.randint(-500, 500), random.randint(0, 200),
                         random.randint(50, 500), random.randint(0, 5),
                         ["in_sync", "holdover", "unsync"][
                             random.choices([0, 1, 2], weights=[80, 15, 5])[0]]))
        # prune simulated metrics history so the DB does not grow unbounded
        con.execute("DELETE FROM latency_log WHERE ts < ?", (now_i - 86400,))
        con.execute("DELETE FROM timesync_reports WHERE ts < ?", (now_i - 86400,))
        con.commit()
        gm_ip = "192.168.1.%d" % random.randint(2, 50)
        add_event("discovery", "cnc", "nodes announced, %d nodes on network" % len(devs),
                  src_ip="192.168.1.%d" % random.randint(2, 50), dst_ip=gm_ip, dest=gm)
        add_event("mqtt", gm, "telemetry {'temp':%s,'press':%s}" % (
                 random.randint(150, 350) / 10, random.randint(990, 1020)),
                 src_ip=gm_ip, dst_ip="192.168.1.%d" % random.randint(2, 50), dest=gm)
        slave = random.choice(devs) if devs else gm
        slave_ip = "192.168.1.%d" % random.randint(2, 50)
        gmid = "80:00:11:ff:fe:%02x:%02x:01" % (random.randint(0, 255), random.randint(0, 255))
        cid = "00:1b:%02x:%02x:%02x:%02x:80:01" % tuple(random.randint(0, 255) for _ in range(4))
        syncd = random.random() < 0.75
        if random.random() < 0.85:
            add_event("ptp", gm, "BMCA: %s elected grandmaster, GM ID %s" % (gm, gmid),
                     src_ip=gm_ip, dst_ip=slave_ip, dest=slave, proto="IEEE 802.1AS")
        if syncd:
            add_event("ptp", gm, "Sync (ClockIdentity %s)" % cid,
                     src_ip=gm_ip, dst_ip=slave_ip, dest=slave, proto="IEEE 802.1AS")
            add_event("ptp", gm, "Follow_Up (precision timestamp)",
                     src_ip=gm_ip, dst_ip=slave_ip, dest=slave, proto="IEEE 802.1AS")
            add_event("ptp", slave, "Delay_Req to master", src_ip=slave_ip,
                     dst_ip=gm_ip, dest=gm, proto="IEEE 802.1AS")
            off = random.randint(-500, 500)
            add_event("ptp", gm, "Delay_Resp: offset from master %d ns (GM ID %s)" %
                     (off, gmid), src_ip=gm_ip, dst_ip=slave_ip, dest=slave,
                     proto="IEEE 802.1AS")
            add_event("ptp", gm, "Sync jitter %d ns" % random.randint(0, 200),
                     src_ip=gm_ip, dst_ip=slave_ip, dest=slave, proto="IEEE 802.1AS")
        add_event("ptp", gm, "gPTP %s offset %d ns, jitter %d ns" %
                  ("in sync" if syncd else "out of sync",
                   random.randint(-300, 300), random.randint(0, 250)),
                 src_ip=gm_ip, dst_ip=slave_ip, dest=gm, proto="IEEE 802.1AS")
        add_event("qos", "cnc", "802.1Q priority 5 traffic class 3 deployed",
                 src_ip="192.168.1.%d" % random.randint(2, 50), dst_ip=gm_ip, dest=gm, proto="IEEE 802.1Q (QoS)")
        add_event("vlan", "cnc", "vlan_id 100 group Control deployed",
                 src_ip="192.168.1.%d" % random.randint(2, 50), dst_ip=gm_ip, dest=gm, proto="IEEE 802.1Q (WVLAN)")
        if random.random() < 0.7:
            add_event("tas", "cnc", "TAS/GCL schedule active, cycle 1 ms",
                     src_ip="192.168.1.%d" % random.randint(2, 50), dst_ip=gm_ip, dest=gm, proto="IEEE 802.1Qbv")
        if random.random() < 0.5:
            add_event("pre", "cnc", "preemption eMAC [7,6,5] pMAC [3,2,1,0]",
                     src_ip="192.168.1.%d" % random.randint(2, 50), dst_ip=gm_ip, dest=gm, proto="IEEE 802.1Qbu")
        add_event("fx", gm, "FX over MQTT C2C field exchange", src_ip=gm_ip,
                 dst_ip="192.168.1.255", dest="")
    finally:
        con.close()


def sim_runner():
    while True:
        try:
            if state.MODE["mode"] == "sim":
                sim_tick()
        except Exception as ex:
            add_event("error", "sim", str(ex))
        time.sleep(2.5)
