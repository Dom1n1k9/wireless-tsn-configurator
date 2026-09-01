#!/usr/bin/env python3
"""WTSN Configurator - web GUI (Python stdlib only).

Deployment vs Simulation mode, devices with TSN functions and grandmaster/slave
role, QoS (802.1Q, priority 0-7), VLAN (ID), TAS/GCL (802.1Qbv), gPTP
(802.1AS), sensors, MQTT, OPC UA FX over MQTT (FXMQTT / C2C Field Exchange),
and a live network/frame monitor. In simulation mode a background thread fabricates
devices, sensors and a realistic frame flow so everything can be exercised without HW.

Run:  python3 webgui.py [--host H] [--port P]   ->  http://127.0.0.1:8000/

Deployment options (env also accepted):
  --host / --port   bind address and port (default 127.0.0.1:8000)
  WTSN_Host         bind address; use 0.0.0.0 to expose on the network
  WTSN_PORT         port
  WTSN_DB           real-mode sqlite path
  WTSN_BROKER       MQTT broker address host:port (default 127.0.0.1:1883)
  WTSN_USER/PASS    broker auth
  WTSN_WEB_USER/PASS  optional HTTP Basic auth for the web UI
TLS is not bundled: put it behind a reverse proxy (nginx/caddy) for production.
"""
import json, os, random, sqlite3, threading, time, socket, struct
import re as _re
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

# --- minimal MQTT 3.1.1 client (stdlib only, no paho dependency) ---
import paho.mqtt.client as mqttlib

class MqttBroker:
    """MQTT 3.1.1 client wrapper (backed by paho-mqtt) with a small,
    synchronous, thread-safe surface used by the rest of the app.

    Methods: connect/subscribe/publish/close/recv_publish.
    recv_publish blocks for the next inbound publication and returns (topic,payload),
    or None when the connection is lost.
    """

    def __init__(self, host="127.0.0.1", port=1883, client_id="wtsn-webgui"):
        self.host, self.port, self.client_id = host, port, client_id
        self.username = os.environ.get("WTSN_USER", "")
        self.password = os.environ.get("WTSN_PASS", "")
        self._paho = None
        self._inbox = deque()
        self._inbox_cv = threading.Condition()
        self._connected = False
        self._lock = threading.Lock()

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        self._connected = (reason_code == 0) if hasattr(reason_code, "is_failure") else (reason_code == 0)

    def connect(self):
        try:
            existing = self._paho
            self._paho = None
            self._connected = False
            if existing:
                try:
                    existing.loop_stop()
                except Exception:
                    pass
                try:
                    existing.disconnect()
                except Exception:
                    pass
            c = mqttlib.Client(mqttlib.CallbackAPIVersion.VERSION2,
                                client_id=self.client_id)
            if self.username:
                c.username_pw_set(self.username, self.password)
            c.on_connect = self._on_connect
            c.on_message = self._on_message
            c.reconnect_delay_set(min_delay=1, max_delay=5)
            c.connect(self.host, self.port, keepalive=60)
            c.loop_start()
            self._paho = c
            self._connected = True
            return True
        except Exception:
            self._paho = None
            self._connected = False
            return False

    def _on_message(self, client, userdata, msg):
        with self._inbox_cv:
            self._inbox.append((msg.topic, msg.payload.decode("utf-8", "replace")))
            self._inbox_cv.notify()

    def subscribe(self, topic, qos=0):
        if not self._paho or not self._connected:
            return False
        try:
            self._paho.subscribe(topic, qos)
            return True
        except Exception:
            return False

    def publish(self, topic, payload, qos=0):
        if not self._paho or not self._connected:
            return False
        try:
            self._paho.publish(topic, payload, qos)
            return True
        except Exception:
            return False

    def recv_publish(self, timeout=None):
        """Block until an inbound publication arrives, then return (topic,payload).
        Returns None when the underlying connection is gone/closing."""
        with self._inbox_cv:

            def closed():
                return not self._paho or (not self._connected and not self._inbox)

            import time as _t

            if timeout is None:
                while not self._inbox:
                    if closed():
                        return None
                    if not self._inbox_cv.wait(timeout=1.0):
                        continue
            else:
                deadline = _t.monotonic() + timeout
                while not self._inbox:
                    if closed():
                        return None
                    remaining = deadline - _t.monotonic()
                    if remaining <= 0:
                        return None
                    self._inbox_cv.wait(timeout=min(1.0, remaining))
            if self._inbox:
                return self._inbox.popleft()
            return None

    def close(self):
        with self._lock:
            if self._paho:
                try:
                    self._paho.loop_stop()
                    self._paho.disconnect()
                except Exception:
                    pass
                self._paho = None
            self._connected = False
            with self._inbox_cv:
                self._inbox.clear()
                self._inbox_cv.notify_all()

def get_self_ip():
    """Return the CNC (this PC) LAN IP used as the ping source address."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
        finally:
            s.close()
    except Exception:
        return "127.0.0.1"

REAL_MQTT = None
def get_real_mqtt(con):
    """Return connected broker client (cached), connecting from settings or env."""
    global REAL_MQTT
    try:
        brk = con.execute("SELECT key,value FROM settings WHERE key='broker'").fetchone()
    except Exception:
        brk = None
    addr = brk["value"] if brk and brk["value"] else os.environ.get("WTSN_BROKER", "127.0.0.1:1883")
    try:
        host, port = addr.rsplit(":", 1)
        port = int(port)
    except Exception:
        host, port = "127.0.0.1", 1883
    if REAL_MQTT is not None:
        if (REAL_MQTT.host, REAL_MQTT.port) != (host, port):
            REAL_MQTT.close(); REAL_MQTT = None
    if REAL_MQTT is None:
        REAL_MQTT = MqttBroker(host, port, "wtsn-webgui")
        if not REAL_MQTT.connect(): return None
    return REAL_MQTT

def mqtt_listener_loop():
    """Background thread: subscribes to status/ack/discover and updates state."""
    brk = host = port = None
    cons = None
    last_db = None
    while not LISTENER_STOP.is_set():
        try:
            wanted_db = DB_REAL if MODE["mode"] == "real" else DB_SIM
            if wanted_db != last_db:
                if cons:
                    try:
                        cons.close()
                    except Exception:
                        pass
                    cons = None
                    if brk:
                        try:
                            brk.close()
                        except Exception:
                            pass
                        brk = None
                last_db = wanted_db
            con = sqlite3.connect(wanted_db, timeout=3)
            con.row_factory = sqlite3.Row
            ensure_schema(con)
            row = con.execute("SELECT value FROM settings WHERE key='broker'").fetchone()
            con.close()
            addr = row["value"] if row and row["value"] else os.environ.get("WTSN_BROKER", "127.0.0.1:1883")
            try:
                h, p = addr.rsplit(":", 1); p = int(p)
            except Exception:
                h, p = "127.0.0.1", 1883
            if brk is None or h != host or p != port:
                if brk: brk.close()
                brk = MqttBroker(h, p, "wtsn-webgui-listener")
                host, port = h, p
            if not brk.connect():
                time.sleep(3); continue
            brk.subscribe("tsn/ack/#")
            brk.subscribe("tsn/status")
            brk.subscribe("tsn/discover")
            brk.subscribe("tsn/fx/#")
            brk.subscribe("tsn/ptp")
            brk.subscribe("tsn/fx/stream")
            brk.subscribe("tsn/sensors")
            if cons is None:
                cons = sqlite3.connect(wanted_db, timeout=3)
                cons.row_factory = sqlite3.Row
                ensure_schema(cons)
            prev_db = wanted_db
            while not LISTENER_STOP.is_set():
                r = brk.recv_publish(timeout=1.0)
                if r is None:
                    # reconnect to re-evaluate mode / broker address if it changed
                    break
                parse_listener_msg(cons, r[0], r[1])
                # re-evaluate mode / broker after each message; switch DB if changed
                cur_db = DB_REAL if MODE["mode"] == "real" else DB_SIM
                if cur_db != prev_db:
                    prev_db = cur_db
                    break
        except Exception:
            time.sleep(3)

def parse_listener_msg(con, topic, payload):
    try:
        j = json.loads(payload.replace("\r", "").replace("\n", "").strip())
    except Exception:
        j = {}
    did = j.get("id", "")
    try:
        if "/status" in topic and did:
            con.execute("UPDATE devices SET status=0,last_seen=strftime('%s','now') WHERE id=?", (did,))
        elif "/discover" in topic and did:
            con.execute("INSERT OR REPLACE INTO devices(id,name,status,last_seen) VALUES(?,?,0,strftime('%s','now'))", (did, did))
        elif "/ack" in topic:
            ok = j.get("ok", False)
            with ACK_LOCK:
                RECENT_ACKS[did] = (ok, time.time())
            if "ip" in j:
                cnc_ip = get_self_ip()
                add_event("mqtt", did,
                          "PONG <- %s: ping reply from %s (%s)" % (did, did, j.get("ip", "?")),
                          src_ip=j.get("ip", ""), dst_ip=cnc_ip, dest=did, proto="MQTT")
                con.execute("UPDATE devices SET ip=?,last_seen=strftime('%s','now') WHERE id=?",
                            (j.get("ip", ""), did))
            else:
                add_event("config", "cnc", "ack %s %s" % (did, "OK" if ok else "FAIL"))
            con.commit()
            return
        elif "/ptp" in topic and did:
            add_event("ptp", did or "broker",
                     "gPTP offset %s ns jitter %s ns state %s" %
                     (j.get("offset_ns", 0), j.get("jitter_ns", 0),
                      ["in sync", "holdover", "unsync"][j.get("state", 2) % 3]),
                     proto="IEEE 802.1AS")
            con.commit()
            return
        elif "/sensors" in topic and did:
            s_list = j.get("sensors", [])
            brief = ", ".join("%s=%s%s" % (s.get("sensor_id", "?"), s.get("value"), s.get("unit", ""))
                            for s in s_list if s.get("sensor_id"))
            if not brief:
                brief = payload
            add_event("sensor", did, "sensors: " + brief)
            dev = con.execute("SELECT id FROM devices WHERE id=?", (did,)).fetchone()
            if not dev:
                con.execute("INSERT OR REPLACE INTO devices(id,name,status,last_seen)"
                           " VALUES(?,?,0,strftime('%s','now'))", (did, did))
            for s in s_list:
                sid = s.get("sensor_id", "")
                typ = s.get("type", 0)
                val = s.get("value", 0)
                unit = s.get("unit", "")
                healthy = s.get("healthy", 1)
                if not sid:
                    continue
                con.execute(
                    "INSERT OR REPLACE INTO sensors(device_id,sensor_id,type,name,"
                    "value,unit,healthy,last_update) "
                    "VALUES(?,?,?,?,?,?,?,strftime('%s','now'))",
                    (did, sid, typ, sid, val, unit, healthy))
            con.commit()
            return
        con.commit()
        add_event(topic.split("/")[0], did or "broker", topic + " <- " + payload)
    except Exception:
        pass

BASE = os.path.dirname(os.path.abspath(__file__))
DB_DIR = os.path.join(BASE, "build")
os.makedirs(DB_DIR, exist_ok=True)
DB_REAL = os.environ.get("WTSN_DB", os.path.join(DB_DIR, "wtsn_gui.db"))
DB_SIM = os.path.join(DB_DIR, "wtsn_sim.db")
PORT = int(os.environ.get("WTSN_PORT", "8000"))

TABLES = ["devices", "device_tsn_features", "qos_configs", "vlan_groups",
          "vlan_members", "tas_schedules", "gcl_entries", "timesync_status",
          "sensors", "preemption_configs", "tsn_streams", "tsn_stream_members",
          "settings", "domains", "config_versions"]

SCHEMA = (
    "CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY,name TEXT,ip TEXT,mac TEXT,"
    "kind INTEGER,firmware TEXT,status INTEGER,last_seen INTEGER,domain TEXT DEFAULT 'default',"
    "heartbeat_at INTEGER DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS domains(id TEXT PRIMARY KEY,name TEXT,description TEXT);"
    "CREATE TABLE IF NOT EXISTS device_tsn_features(device_id TEXT,feature TEXT);"
    "CREATE TABLE IF NOT EXISTS qos_configs(device_id TEXT,priority INTEGER,traffic_class "
    "INTEGER,bandwidth_kbps INTEGER,latency_ms INTEGER,preemption INTEGER);"
    "CREATE TABLE IF NOT EXISTS preemption_configs(device_id TEXT PRIMARY KEY,preemption "
    "INTEGER,emac TEXT,pmac TEXT);"
    "CREATE TABLE IF NOT EXISTS vlan_groups(id TEXT PRIMARY KEY,name TEXT,vlan_id INTEGER);"
    "CREATE TABLE IF NOT EXISTS vlan_members(group_id TEXT,device_id TEXT);"
    "CREATE TABLE IF NOT EXISTS tas_schedules(id TEXT PRIMARY KEY,name TEXT,cycle_time_ns "
    "INTEGER,deploy_target TEXT);"
    "CREATE TABLE IF NOT EXISTS gcl_entries(schedule_id TEXT,\"index\" INTEGER,gate_state "
    "INTEGER,duration_ns INTEGER);"
    "CREATE TABLE IF NOT EXISTS timesync_status(id TEXT,mode INTEGER,grandmaster TEXT,"
    "offset_ns INTEGER,quality INTEGER,jitter_ns INTEGER DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS sensors(device_id TEXT,sensor_id TEXT,type INTEGER,name TEXT,"
    "value REAL,unit TEXT,healthy INTEGER,last_update INTEGER,"
    "PRIMARY KEY(device_id,sensor_id));"
    "CREATE TABLE IF NOT EXISTS tsn_streams(stream_id TEXT PRIMARY KEY,name TEXT,talker TEXT,"
    "vlan_id INTEGER,max_latency_ns INTEGER,max_interval_ns INTEGER,priority INTEGER,"
    "data_frame_prio INTEGER,status INTEGER CHECK(status IN (0,1,2,3)),comment TEXT);"
    "CREATE TABLE IF NOT EXISTS tsn_stream_members(stream_id TEXT,role TEXT,device_id TEXT);"
    "CREATE TABLE IF NOT EXISTS timesync_reports(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "device_id TEXT,ts INTEGER,offset_ns INTEGER,jitter_ns INTEGER,packet_count INTEGER,"
    "packet_loss INTEGER,status TEXT);"
    "CREATE TABLE IF NOT EXISTS trace_log(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "ts INTEGER,type INTEGER,source TEXT,line TEXT);"
    "CREATE TABLE IF NOT EXISTS config_versions(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name TEXT,device_id TEXT,created_at INTEGER,payload TEXT);"
    "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT);"
)

def ensure_schema(con):
    con.executescript(SCHEMA)
    con.commit()
    for tbl, col in (("devices", "domain"), ("devices", "heartbeat_at"),
                     ("timesync_status", "jitter_ns")):
        try:
            cols = [r[1] for r in con.execute("PRAGMA table_info(%s)" % tbl)]
            if col not in cols:
                con.execute("ALTER TABLE %s ADD COLUMN %s" % (tbl, col))
        except Exception:
            pass
    con.commit()
    try:
        # Older DBs created 'sensors' without a primary key, so INSERT OR REPLACE
        # appended duplicate rows. Rebuild a deduplicated table if needed.
        rows = con.execute("PRAGMA table_info(sensors)").fetchall()
        if rows and not any(r[5] for r in rows):
            con.execute("""CREATE TABLE sensors_tmp(
                device_id TEXT,sensor_id TEXT,type INTEGER,name TEXT,
                value REAL,unit TEXT,healthy INTEGER,last_update INTEGER,
                PRIMARY KEY(device_id,sensor_id))""")
            con.execute("""
                INSERT OR REPLACE INTO sensors_tmp(device_id,sensor_id,type,name,value,unit,healthy,last_update)
                SELECT device_id,sensor_id,MAX(type),MAX(name),value,MAX(unit),MAX(healthy),MAX(last_update)
                FROM sensors GROUP BY device_id,sensor_id""")
            con.execute("DROP TABLE sensors")
            con.execute("ALTER TABLE sensors_tmp RENAME TO sensors")
            con.commit()
    except Exception:
        pass

PROFILES = [(0, "esp32", "192.168.1.10", "ESP32 Gateway"),
            (2, "rpi", "192.168.1.20", "Raspberry Pi"),
            (0, "linux", "192.168.1.30", "Linux Node"),
            (3, "stm32", "192.168.1.40", "STM32 Sensor"),
            (0, "nxp", "192.168.1.50", "NXP Node")]
TSN_FUNCS = ["802.1Q QoS", "802.1Q VLAN", "gPTP 802.1AS", "802.1Qbv TAS",
             "802.1Qbu Preemption", "OPC UA", "OPC UA PubSub", "FX Multicast"]

EVENTS = deque(maxlen=400)
EVENT_LOCK = threading.Lock()
MODE = {"mode": "sim"}
LISTENER_STOP = threading.Event()
RECENT_ACKS = {}
ACK_LOCK = threading.Lock()
SIM_USER_DEVICES = set()
SIM_USER_DEVICES_LOCK = threading.Lock()
OFFLINE_AFTER = 20
SIM_STABLE_DEVICES = None
SIM_STABLE_LOCK = threading.Lock()


def connect():
    con = sqlite3.connect(DB_SIM if MODE["mode"] == "sim" else DB_REAL, timeout=3)
    con.row_factory = sqlite3.Row
    try:
        con.execute("PRAGMA journal_mode=WAL")
        con.execute("PRAGMA busy_timeout=5000")
        con.execute("PRAGMA synchronous=NORMAL")
    except sqlite3.Error:
        pass
    ensure_schema(con)
    return con


_PROTO_MAP = {"mqtt": "MQTT", "fx": "FXMQTT", "config": "CNC", "tsn": "TSN",
              "error": "ERR", "frame": "RAW", "discovery": "mDNS",
              "ptp": "IEEE 802.1AS", "qos": "IEEE 802.1Q (QoS)", "vlan": "IEEE 802.1Q (WVLAN)",
              "tas": "IEEE 802.1Qbv", "pre": "IEEE 802.1Qbu"}

def add_event(kind, source, msg, src_ip="", dst_ip="", dest="", proto=""):
    if not proto:
        if kind == "fx":
            proto = "FXMQTT"
        else:
            proto = _PROTO_MAP.get(kind, kind.upper())
    with EVENT_LOCK:
        EVENTS.appendleft({"ts": time.strftime("%H:%M:%S"), "kind": kind,
                         "source": source, "msg": msg,
                         "src_ip": src_ip, "dst_ip": dst_ip, "dest": dest,
                         "proto": proto})


def load_all():
    out = {}
    con = connect()
    try:
        for t in TABLES:
            try:
                out[t] = [dict(r) for r in con.execute("SELECT * FROM %s" % t)]
            except sqlite3.Error:
                out[t] = []
        # stale detection: a device that has not reported within OFFLINE_AFTER
        # seconds is shown as offline even if it is still marked online in the DB.
        now = int(time.time())
        for d in out.get("devices", []):
            ls = d.get("last_seen") or 0
            if ls and (now - ls) > OFFLINE_AFTER and MODE["mode"] == "real":
                d["status"] = 1
    finally:
        con.close()
    return out


def get_events():
    with EVENT_LOCK:
        return {"mode": MODE["mode"], "events": list(EVENTS)[:300]}


def _gen_stable_devices():
    """Generate a fixed simulated device set once. Reused every tick so the device
    list stays constant while sensor values / status continue to drift."""
    devs = []
    n = random.randint(3, 6)
    per = {}
    for i in range(1, n + 1):
        kind, base, ip, name = random.choice(PROFILES)
        per.setdefault(base, 0)
        per[base] += 1
        did = "%s-%02d" % (base, per[base])
        devs.append({
            "id": did, "name": name, "ip": ip, "mac": "AA:BB:CC:%02d:%02d" % (i, kind),
            "kind": kind, "firmware": "%d.%d.%d" % (random.randint(1, 5),
                     random.randint(0, 9), random.randint(0, 9)),
            "tsn": random.sample(TSN_FUNCS, random.randint(4, len(TSN_FUNCS))),
        })
    return devs


def sim_tick():
    global SIM_STABLE_DEVICES
    con = connect()
    try:
        with SIM_USER_DEVICES_LOCK:
            keep = list(SIM_USER_DEVICES)
        kept = {}
        for k in keep:
            row = con.execute("SELECT * FROM devices WHERE id=?", (k,)).fetchone()
            if row:
                kept[k] = dict(row)
            else:
                with SIM_USER_DEVICES_LOCK:
                    SIM_USER_DEVICES.discard(k)
        saved_ts = con.execute("SELECT * FROM timesync_status WHERE id='main'").fetchone()
        saved_ts = dict(saved_ts) if saved_ts else None
        saved_nodes = con.execute("SELECT * FROM settings WHERE key='sync_nodes'").fetchone()
        saved_nodes = dict(saved_nodes) if saved_nodes else None
        # Config tables are user configuration and must NOT be regenerated/destroyed by
        # the simulator. Only simulated runtime data is rebuilt each tick, using a stable
        # simulated device set (generated once) so devices do not appear/disappear each tick.
        for t in ["devices", "sensors", "timesync_status",
                  "device_tsn_features"]:
            con.execute("DELETE FROM %s" % t)
        for k, kv in kept.items():
            con.execute("INSERT OR REPLACE INTO devices(id,name,ip,mac,kind,firmware,status,"
                        "last_seen,domain) VALUES(?,?,?,?,?,?,?,?,?)",
                        (k, kv.get("name", ""), kv.get("ip", ""), kv.get("mac", ""),
                         kv.get("kind", 0), kv.get("firmware", ""), kv.get("status", 0),
                         kv.get("last_seen", int(time.time())), kv.get("domain", "default")))
        with SIM_STABLE_LOCK:
            if SIM_STABLE_DEVICES is None:
                SIM_STABLE_DEVICES = _gen_stable_devices()
            stable = list(SIM_STABLE_DEVICES)
        devs = [d["id"] for d in stable]
        for sd in stable:
            did = sd["id"]
            con.execute("INSERT INTO devices(id,name,ip,mac,kind,firmware,status,last_seen,domain)"
                       " VALUES(?,?,?,?,?,?,0,strftime('%s','now'),'default')",
                       (did, sd["name"], sd["ip"], sd["mac"], sd["kind"],
                        sd["firmware"]))
            for f in sd["tsn"]:
                con.execute("INSERT INTO device_tsn_features(device_id,feature) VALUES(?,?)",
                            (did, f))
            for sid, typ, unit, basev in (("temp1", 0, "C", 25.0), ("press1", 1, "hPa", 1005.0),
                                           ("imu1", 2, "g", 0.3), ("gpio1", 4, "V", 1.0)):
                con.execute("INSERT OR REPLACE INTO sensors(device_id,sensor_id,type,name,"
                            "value,unit,healthy,last_update) VALUES(?,?,?,?,?,?,1,strftime('%s','now'))",
                            (did, sid, typ, sid, basev, unit))
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
                 src_ip=gm_ip, dst_ip=slave_ip, dest=slave, proto="IEEE 802.1AS")
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
            if MODE["mode"] == "sim":
                sim_tick()
        except Exception as ex:
            add_event("error", "sim", str(ex))
        time.sleep(2.5)


def clamp(v, lo, hi):
    try:
        return max(lo, min(hi, int(v)))
    except (TypeError, ValueError):
        return lo


def run_action(act, body):
    con = connect()
    try:
        if act == "set_mode":
            MODE["mode"] = body.get("mode", "sim") if body.get("mode") in ("sim", "real") else MODE["mode"]
            EVENTS.clear()
            add_event("config", "cnc",
                     "mode = SIMULATION" if MODE["mode"] == "sim" else "mode = REAL (waiting for real devices)")
            return {"ok": True, "msg": "mode = " + MODE["mode"]}
        if act == "save_domain":
            if not body.get("id"):
                return {"ok": False, "msg": "domain id required"}
            con.execute("INSERT OR REPLACE INTO domains(id,name,description) VALUES(?,?,?)",
                        (body["id"], body.get("name", body["id"]), body.get("description", "")))
            con.commit()
            return {"ok": True, "msg": "domain saved"}
        if act == "delete_domain":
            did = body.get("id")
            if did == "default":
                return {"ok": False, "msg": "cannot delete default domain"}
            con.execute("DELETE FROM domains WHERE id=?", (did,))
            con.execute("UPDATE devices SET domain='default' WHERE domain=?", (did,))
            con.commit()
            return {"ok": True, "msg": "domain deleted"}
        if act == "assign_domain":
            did = body.get("device_id"); dom = body.get("domain")
            if not did or not dom:
                return {"ok": False, "msg": "device and domain required"}
            con.execute("UPDATE devices SET domain=? WHERE id=?", (dom, did))
            con.commit()
            return {"ok": True, "msg": "device assigned"}
        if act == "set_server":
            server_type = body.get("type", "node")
            did = body.get("id", "")
            broker = body.get("broker", "127.0.0.1:1883")
            con.execute("INSERT OR REPLACE INTO settings(key,value) VALUES('server_type',?)",
                        (server_type,))
            con.execute("INSERT OR REPLACE INTO settings(key,value) VALUES('server_id',?)",
                        (did,))
            con.execute("INSERT OR REPLACE INTO settings(key,value) VALUES('broker',?)",
                        (broker,))
            con.commit()
            add_event("config", "cnc",
                      "server = " + ("PC" if server_type == "pc" else "node " + did))
            return {"ok": True, "msg": ("server = PC" if server_type == "pc"
                                        else "server = node " + did)}
        if act == "save_devices":
            for i in body.get("delete") or []:
                for t, c in (("devices", "id"), ("qos_configs", "device_id"),
                             ("vlan_members", "device_id"), ("sensors", "device_id"),
                             ("device_tsn_features", "device_id")):
                    con.execute("DELETE FROM %s WHERE %s=?" % (t, c), (i,))
                with SIM_USER_DEVICES_LOCK:
                    SIM_USER_DEVICES.discard(i)
                if MODE["mode"] == "real":
                    b = get_real_mqtt(con)
                    if b:
                        b.publish("tsn/cmd/%s/reset" % i, "{}")
                add_event("config", "cnc", "removed " + i)
            dev = body.get("device") or {}
            if dev.get("id"):
                con.execute("INSERT OR REPLACE INTO devices(id,name,ip,mac,kind,firmware,status,"
                            "last_seen,domain) VALUES(?,?,?,?,?,?,?,strftime('%s','now'),?)",
                            (dev["id"], dev.get("name", ""), dev.get("ip", ""),
                             dev.get("mac", ""), clamp(dev.get("kind", 0), 0, 3),
                             dev.get("firmware", ""), clamp(dev.get("status", 0), 0, 2),
                             dev.get("domain", "default")))
                con.execute("DELETE FROM device_tsn_features WHERE device_id=?", (dev["id"],))
                for f in dev.get("tsn", []) or []:
                    con.execute("INSERT INTO device_tsn_features(device_id,feature) VALUES(?,?)",
                                (dev["id"], f))
                if MODE["mode"] == "sim":
                    with SIM_USER_DEVICES_LOCK:
                        SIM_USER_DEVICES.add(dev["id"])
                add_event("config", "cnc", "device %s updated: %s" % (dev["id"],
                         ",".join(dev.get("tsn") or [])))
            con.commit()
            return {"ok": True, "msg": "Devices updated"}
        if act == "set_role":
            role = body.get("role")
            did = body.get("id")
            if role == "grandmaster":
                con.execute("UPDATE timesync_status SET grandmaster=? WHERE id='main'", (did,))
                con.commit()
            add_event("config", "cnc", "%s -> %s" % (did, role))
            return {"ok": True, "msg": "role set"}
        if act == "save_qos":
            con.execute("INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
                        "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?)",
                        (body.get("device_id"), clamp(body.get("priority", 5), 0, 7),
                         clamp(body.get("traffic_class", 1), 0, 3),
                         clamp(body.get("bandwidth_kbps", 1000), 1, 1000000),
                         clamp(body.get("latency_ms", 1), 0, 10000),
                         clamp(body.get("preemption", 0), 0, 2)))
            con.commit()
            add_event("qos", "cnc", "802.1Q priority %s -> %s" % (body.get("priority"),
                     body.get("device_id")), src_ip="", dst_ip="", dest=body.get("device_id"), proto="IEEE 802.1Q (QoS)")
            return {"ok": True, "msg": "QoS saved"}
        if act == "delete_qos":
            con.execute("DELETE FROM qos_configs WHERE device_id=?", (body.get("device_id"),))
            con.commit()
            return {"ok": True, "msg": "QoS deleted"}
        if act == "save_preemption":
            e = body.get("emac") or ""
            p = body.get("pmac") or ""
            if body.get("preemption", 0) == 0:
                e = p = ""
            con.execute("INSERT OR REPLACE INTO preemption_configs(device_id,preemption,"
                        "emac,pmac) VALUES(?,?,?,?)",
                        (body.get("device_id"), clamp(body.get("preemption", 0), 0, 1),
                         e, p))
            con.commit()
            add_event("pre", "cnc", "802.1Qbu preemption %s -> %s (eMAC [%s] pMAC [%s])" %
                     (body.get("device_id"), body.get("preemption"), e, p),
                     src_ip="", dst_ip="", dest=body.get("device_id"), proto="IEEE 802.1Qbu")
            return {"ok": True, "msg": "Preemption saved"}
        if act == "delete_preemption":
            con.execute("DELETE FROM preemption_configs WHERE device_id=?",
                        (body.get("device_id"),))
            con.commit()
            return {"ok": True, "msg": "Preemption deleted"}
        if act == "save_vlan":
            vlan = clamp(body.get("vlan_id", 1), 1, 4094)
            gid = body.get("id") or ("grp%d" % vlan)
            con.execute("INSERT OR REPLACE INTO vlan_groups(id,name,vlan_id) VALUES(?,?,?)",
                        (gid, body.get("name", ""), vlan))
            con.commit()
            add_event("vlan", "cnc", "VLAN group %s created (vlan_id %d, name %s)" %
                      (gid, vlan, body.get("name", "")),
                     src_ip="", dst_ip="", dest="", proto="IEEE 802.1Q (WVLAN)")
            return {"ok": True, "msg": "VLAN group saved"}
        if act == "delete_vlan":
            con.execute("DELETE FROM vlan_groups WHERE id=?", (body.get("id"),))
            con.execute("DELETE FROM vlan_members WHERE group_id=?", (body.get("id"),))
            con.commit()
            return {"ok": True, "msg": "VLAN deleted"}
        if act == "save_member":
            gid = body.get("group_id")
            grp = con.execute("SELECT vlan_id,name FROM vlan_groups WHERE id=?",
                             (gid,)).fetchone()
            gtag = grp["vlan_id"] if grp else "?"
            gname = grp["name"] if grp else gid
            if body.get("set_members") is not None:
                picked = [x for x in body.get("set_members") if x]
                con.execute("DELETE FROM vlan_members WHERE group_id=?", (gid,))
                for dev in picked:
                    con.execute("INSERT OR REPLACE INTO vlan_members(group_id,device_id) VALUES(?,?)",
                                (gid, dev))
                con.commit()
                add_event("vlan", "cnc", "devices %s propagate VLAN tag %d (%s) via 802.1Q" %
                          (",".join(picked) if picked else "-", gtag, gname), src_ip="",
                         dst_ip="", dest=", ".join(picked), proto="IEEE 802.1Q (WVLAN)")
                return {"ok": True, "msg": "members updated"}
            dev = body.get("device_id", "")
            remove = bool(body.get("remove"))
            if remove:
                con.execute("DELETE FROM vlan_members WHERE group_id=? AND device_id=?",
                            (gid, dev))
                msg = "Member removed"
            elif dev:
                con.execute("INSERT OR REPLACE INTO vlan_members(group_id,device_id) VALUES(?,?)",
                            (gid, dev))
                msg = "Member added"
            else:
                con.execute("DELETE FROM vlan_members WHERE group_id=?", (gid,))
                msg = "All members removed from " + str(gid)
            con.commit()
            add_event("vlan", "cnc", "%s %s (VLAN tag %d, %s)" % (msg, dev, gtag, gname),
                     src_ip="", dst_ip="", dest=dev or gid, proto="IEEE 802.1Q (WVLAN)")
            return {"ok": True, "msg": msg}
        if act == "save_tas":
            cid = body.get("id") or ("sched%d" % int(time.time()))
            con.execute("INSERT OR REPLACE INTO tas_schedules(id,name,cycle_time_ns,deploy_target)"
                        " VALUES(?,?,?,?)",
                        (cid, body.get("name", ""), clamp(body.get("cycle_time_ns", 1000000), 0, 10**12),
                         body.get("deploy_target", "")))
            con.execute("DELETE FROM gcl_entries WHERE schedule_id=?", (cid,))
            for i, e in enumerate(body.get("gcl") or []):
                con.execute("INSERT INTO gcl_entries(schedule_id,\"index\",gate_state,duration_ns)"
                            " VALUES(?,?,?,?)",
                            (cid, i, clamp(e.get("gate_state", 0), 0, 255),
                             clamp(e.get("duration_ns", 0), 0, 10**12)))
            con.commit()
            add_event("tas", "cnc", "802.1Qbv TAS %s -> %s (GCL %d entries)" %
                     (cid, body.get("deploy_target"), len(body.get("gcl") or [])),
                     src_ip="", dst_ip="", dest=body.get("deploy_target", ""), proto="IEEE 802.1Qbv")
            return {"ok": True, "msg": "TAS saved"}
        if act == "delete_tas":
            con.execute("DELETE FROM tas_schedules WHERE id=?", (body.get("id"),))
            con.execute("DELETE FROM gcl_entries WHERE schedule_id=?", (body.get("id"),))
            con.commit()
            return {"ok": True, "msg": "TAS deleted"}
        if act == "save_timesync":
            con.execute("INSERT OR REPLACE INTO timesync_status(id,mode,grandmaster,offset_ns,"
                        "quality) VALUES('main',?,?,?,?)",
                        (clamp(body.get("mode", 0), 0, 3), body.get("grandmaster", ""),
                         clamp(body.get("offset_ns", 0), -(10**12), 10**12),
                         clamp(body.get("quality", 0), 0, 100)))
            con.execute("INSERT OR REPLACE INTO settings(key,value) VALUES('sync_nodes',?)",
                        (",".join(body.get("nodes") or []),))
            con.commit()
            add_event("ptp", "cnc", "802.1AS gPTP sync saved (GM %s)" %
                     body.get("grandmaster"), src_ip="", dst_ip="",
                     dest=body.get("grandmaster", ""), proto="IEEE 802.1AS")
            return {"ok": True, "msg": "Time sync saved"}
        if act == "exec_all":
            svr = dict((r["key"], r["value"])
                       for r in con.execute("SELECT key,value FROM settings"))
            target = svr.get("server_id", "") or (svr.get("server_type", "pc"))
            add_event("config", "cnc", "EXECUTING settings on controller/server=" + str(target))
            broker = get_real_mqtt(con)
            n_pub = 0
            snapshots = {}
            for r in con.execute("SELECT id FROM devices"):
                did = r["id"]
                if not broker:
                    break
                snap = {"id": did}
                qos = con.execute("SELECT * FROM qos_configs WHERE device_id=?", (did,)).fetchone()
                if qos:
                    snap["priority"] = qos["priority"]
                    snap["traffic_class"] = qos["traffic_class"]
                    snap["bandwidth_kbps"] = qos["bandwidth_kbps"]
                    snap["latency_ms"] = qos["latency_ms"]
                    snap["preemption"] = qos["preemption"] or 0
                v = con.execute("SELECT v.* FROM vlan_members gr JOIN vlan_groups v ON "
                               "v.id=gr.group_id WHERE gr.device_id=?", (did,)).fetchone()
                if v:
                    snap["vlan_id"] = v["vlan_id"]
                    snap["group"] = v["name"]
                pre = con.execute("SELECT * FROM preemption_configs WHERE device_id=?", (did,)).fetchone()
                if pre:
                    snap["preemption"] = pre["preemption"]
                ts = con.execute("SELECT * FROM timesync_status WHERE id='main'", ()).fetchone()
                if ts:
                    gm = ts["grandmaster"]
                    # GM device becomes grandmaster (mode 1); other devices are slaves (mode 2)
                    if gm and gm != "PC":
                        snap["timesync_mode"] = 1 if did == gm else 2
                        snap["grandmaster"] = gm
                    else:
                        snap["timesync_mode"] = ts["mode"]
                        snap["grandmaster"] = gm
                tas = con.execute("SELECT * FROM tas_schedules WHERE 1 LIMIT 1",
                                 ()).fetchone()
                if tas:
                    snap["tas_cycle_ns"] = tas["cycle_time_ns"]
                    gcl = con.execute(
                        "SELECT gate_state,duration_ns FROM gcl_entries "
                        "WHERE schedule_id=? ORDER BY \"index\"", (tas["id"],)).fetchall()
                    if gcl:
                        snap["gcl"] = [{"gate_state": g["gate_state"],
                                         "duration_ns": g["duration_ns"]} for g in gcl]
                snapshots[did] = json.dumps(snap)
                broker.publish("tsn/cmd/%s/apply" % did, snapshots[did])
                broker.publish("tsn/cmd/%s/status" % did, "1")
                n_pub += 1
                add_event("fxmqtt", "cnc", "apply sent tsn/cmd/%s/apply" % did)
            # 802.1Qcc streams are carried over our FXMQTT field-exchange channel
            stream_rows = con.execute("SELECT * FROM tsn_streams").fetchall()
            for sr in stream_rows:
                sid = sr["stream_id"]
                memb = con.execute(
                    "SELECT role,device_id FROM tsn_stream_members WHERE stream_id=?",
                    (sid,)).fetchall()
                talker = ""
                listeners = []
                for m in memb:
                    if m["role"] == "talker": talker = m["device_id"]
                    elif m["role"] == "listener": listeners.append(m["device_id"])
                sd = {"stream_id": sid, "name": sr["name"], "talker": talker,
                      "listeners": listeners, "vlan_id": sr["vlan_id"],
                      "max_latency_ns": sr["max_latency_ns"],
                      "max_interval_ns": sr["max_interval_ns"],
                      "priority": sr["priority"],
                      "data_frame_prio": sr["data_frame_prio"]}
                payload = json.dumps(sd)
                if talker:
                    broker.publish("tsn/fx/stream", payload)
                    broker.publish("tsn/cmd/%s/stream" % talker, payload)
                    add_event("fxmqtt", "cnc",
                            "stream %s published on tsn/fx/stream -> %s" % (sid, talker))
                    n_pub += 1
                for l in listeners:
                    broker.publish("tsn/cmd/%s/stream" % l, payload)
                    n_pub += 1
            if not broker:
                return {"ok": False, "msg": "MQTT broker not reachable (" + str(target) + ")"}
            # wait up to ~2s for acks; retry once for non-acknowledged devices
            retried = []
            pending = []
            end = time.time() + 2.0
            while time.time() < end:
                with ACK_LOCK:
                    acked = set(RECENT_ACKS.keys())
                pending = [r["id"] for r in con.execute("SELECT id FROM devices")
                           if r["id"] not in acked]
                if not pending:
                    break
                time.sleep(0.3)
            for did in pending:
                if did in snapshots:
                    broker.publish("tsn/cmd/%s/apply" % did, snapshots[did])
                    retried.append(did)
                    add_event("fxmqtt", "cnc", "retry tsn/cmd/%s/apply" % did)
            msg = "Sent /apply to %d device(s) via MQTT" % n_pub
            if retried:
                msg += "; %d retried (no ack)" % len(retried)
            return {"ok": True, "msg": msg}
        if act == "set_wifi":
            ssid = body.get("ssid", "")
            pw = body.get("pass", "")
            if MODE["mode"] == "real":
                b = get_real_mqtt(con)
                if b:
                    did = body.get("device_id", "")
                    if did:
                        b.publish("tsn/cmd/%s/wifi" % did,
                                  json.dumps({"ssid": ssid, "pass": pw}))
                        add_event("config", "cnc", "wifi cmd -> %s (%s)" % (did, ssid))
                        return {"ok": True, "msg": "WiFi command sent to " + did}
                    return {"ok": False, "msg": "device_id required"}
                return {"ok": False, "msg": "MQTT broker not reachable"}
            return {"ok": True, "msg": "WiFi set (simulation; sent on Real mode)"}
        if act == "save_stream":
            sid = body.get("stream_id") or ("stream-%d" % int(time.time()))
            talker = body.get("talker", "")
            listeners = body.get("listeners") or []
            if talker:
                con.execute("INSERT OR REPLACE INTO tsn_streams(stream_id,name,talker,"
                            "vlan_id,max_latency_ns,max_interval_ns,priority,"
                            "data_frame_prio,status,comment) VALUES(?,?,?,?,?,?,?,?,0,?)",
                            (sid, body.get("name", ""), talker,
                             clamp(body.get("vlan_id", 0), 0, 4094),
                             clamp(body.get("max_latency_ns", 1000000), 1, 10**12),
                             clamp(body.get("max_interval_ns", 100000), 1, 10**12),
                             clamp(body.get("priority", 5), 0, 7),
                             clamp(body.get("data_frame_prio", 5), 0, 7),
                             body.get("comment", "")))
                con.execute("DELETE FROM tsn_stream_members WHERE stream_id=?", (sid,))
                con.execute("INSERT INTO tsn_stream_members(stream_id,role,device_id) "
                            "VALUES(?,?,?)", (sid, "talker", talker))
                for l in listeners:
                    con.execute("INSERT INTO tsn_stream_members(stream_id,role,device_id) "
                               "VALUES(?,?,?)", (sid, "listener", l))
                con.commit()
                add_event("config", "cnc", "stream %s talker=%s listeners=%d" %
                         (sid, talker, len(listeners)))
                return {"ok": True, "msg": "Stream saved"}
            return {"ok": False, "msg": "talker required"}
        if act == "delete_stream":
            sid = body.get("stream_id", "")
            con.execute("DELETE FROM tsn_streams WHERE stream_id=?", (sid,))
            con.execute("DELETE FROM tsn_stream_members WHERE stream_id=?", (sid,))
            con.commit()
            return {"ok": True, "msg": "Stream deleted"}
        if act == "deploy_stream":
            sid = body.get("stream_id", "")
            con.execute("UPDATE tsn_streams SET status=1 WHERE stream_id=?", (sid,))
            con.commit()
            if MODE["mode"] == "real":
                broker = get_real_mqtt(con)
                if broker:
                    sr = con.execute("SELECT * FROM tsn_streams WHERE stream_id=?",
                                    (sid,)).fetchone()
                    if sr:
                        memb = con.execute(
                            "SELECT role,device_id FROM tsn_stream_members WHERE stream_id=?",
                            (sid,)).fetchall()
                        talker = next((m["device_id"] for m in memb if m["role"] == "talker"), "")
                        listeners = [m["device_id"] for m in memb if m["role"] == "listener"]
                        pl = json.dumps({"stream_id": sid, "name": sr["name"],
                                        "talker": talker, "listeners": listeners,
                                        "vlan_id": sr["vlan_id"],
                                        "max_latency_ns": sr["max_latency_ns"],
                                        "max_interval_ns": sr["max_interval_ns"],
                                        "priority": sr["priority"],
                                        "data_frame_prio": sr["data_frame_prio"]})
                        if talker:
                            broker.publish("tsn/fx/stream", pl)
                            broker.publish("tsn/cmd/%s/stream" % talker, pl)
                        for l in listeners:
                            broker.publish("tsn/cmd/%s/stream" % l, pl)
                else:
                    return {"ok": False, "msg": "MQTT broker not reachable"}
            add_event("config", "cnc", "802.1Qcc stream %s deployed via FXMQTT" % sid)
            return {"ok": True, "msg": "Stream deployed via FXMQTT"}
        if act == "deploy_all_streams":
            con.execute("UPDATE tsn_streams SET status=1")
            con.commit()
            n = con.execute("SELECT COUNT(*) FROM tsn_streams").fetchone()[0]
            if MODE["mode"] == "real":
                broker = get_real_mqtt(con)
                if broker:
                    for sr in con.execute("SELECT * FROM tsn_streams").fetchall():
                        memb = con.execute(
                            "SELECT role,device_id FROM tsn_stream_members WHERE stream_id=?",
                            (sr["stream_id"],)).fetchall()
                        talker = next((m["device_id"] for m in memb if m["role"] == "talker"), "")
                        listeners = [m["device_id"] for m in memb if m["role"] == "listener"]
                        pl = json.dumps({"stream_id": sr["stream_id"], "name": sr["name"],
                                        "talker": talker, "listeners": listeners,
                                        "vlan_id": sr["vlan_id"],
                                        "max_latency_ns": sr["max_latency_ns"],
                                        "max_interval_ns": sr["max_interval_ns"],
                                        "priority": sr["priority"],
                                        "data_frame_prio": sr["data_frame_prio"]})
                        if talker:
                            broker.publish("tsn/fx/stream", pl)
                            broker.publish("tsn/cmd/%s/stream" % talker, pl)
                        for l in listeners:
                            broker.publish("tsn/cmd/%s/stream" % l, pl)
                else:
                    return {"ok": False, "msg": "MQTT broker not reachable"}
            add_event("config", "cnc", "802.1Qcc all %d streams deployed via FXMQTT" % n)
            return {"ok": True, "msg": "%d streams deployed via FXMQTT" % n}
        if act == "fx_send":
            b = get_real_mqtt(con) if MODE["mode"] == "real" else None
            add_event("fx", body.get("source", "cnc"),
                      ("tsn/fx/field <- " + body.get("msg", "")))
            if b:
                b.publish("tsn/fx/field", body.get("msg", ""))
                return {"ok": True, "msg": "FX published on broker"}
            return {"ok": True, "msg": "FX sent (no broker / simulation)"}
        if act == "ping_device":
            did = body.get("id", "")
            if not did:
                return {"ok": False, "msg": "missing device id"}
            b = get_real_mqtt(con) if MODE["mode"] == "real" else None
            if not b:
                add_event("config", "cnc", "identify %s (no broker)" % did)
                return {"ok": False, "msg": "no broker in real mode"}
            b.publish("tsn/cmd/%s/ping" % did, "1")
            cnc_ip = get_self_ip()
            add_event("mqtt", "cnc", "PING -> %s" % did, src_ip=cnc_ip, dst_ip="",
                      dest=did, proto="MQTT")
            return {"ok": True, "msg": "ping sent to " + did}
        if act == "clear_events":
            EVENTS.clear()
            return {"ok": True, "msg": "monitor cleared"}
        if act == "rollback_version":
            vid = body.get("id")
            if vid is None:
                return {"ok": False, "msg": "select a version"}
            row = con.execute("SELECT payload,device_id FROM config_versions WHERE id=?",
                             (int(vid),)).fetchone()
            if not row:
                return {"ok": False, "msg": "version not found"}
            payload, dev = row[0], row[1]
            try:
                data = json.loads(payload)
            except Exception:
                data = None
            if isinstance(data, dict) and data.get("devices") is not None:
                # stored as full JSON (webgui snapshots)
                for t in ("devices", "device_tsn_features", "qos_configs", "preemption_configs",
                          "vlan_groups", "vlan_members", "tas_schedules", "gcl_entries",
                          "timesync_status", "tsn_streams", "tsn_stream_members", "settings"):
                    rows = data.get(t)
                    con.execute("DELETE FROM %s" % t)
                    for r in rows or []:
                        if not isinstance(r, dict):
                            continue
                        cols = [k for k in r if isinstance(r[k], (str, int, float))]
                        if not cols:
                            continue
                        con.execute("INSERT INTO %s(%s) VALUES(%s)" % (t, ",".join(cols),
                                   ",".join(["?"] * len(cols))), [r[k] for k in cols])
                con.commit()
                return {"ok": True, "msg": "rolled back to version " + str(vid)}
            if dev:
                con.execute("DELETE FROM qos_configs WHERE device_id=?", (dev,))
                con.execute("DELETE FROM vlan_members WHERE device_id=?", (dev,))
                con.execute("DELETE FROM tsn_stream_members WHERE device_id=?", (dev,))
            else:
                for t in ("qos_configs", "vlan_groups", "vlan_members", "tsn_streams",
                          "tsn_stream_members", "tas_schedules", "gcl_entries",
                          "preemption_configs", "device_tsn_features"):
                    con.execute("DELETE FROM %s" % t)
            # fall back to token replay for C-generated payloads
            import re as _re2
            for tok in re.findall(r"\S+", payload or ""):
                if tok.startswith("qos:p="):
                    mm = _re2.match(r"qos:p=(\d+):bw=(\d+):lat=(\d+):pre=(\d+)", tok)
                    if mm:
                        p, bw, lat, pre = map(int, mm.groups())
                        con.execute("INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
                                    "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?)",
                                  (dev or "default", p, p, bw, lat, pre))
                elif tok.startswith("qos:"):
                    mm = _re2.match(r"qos:([^:]+):p=(\d+):bw=(\d+):lat=(\d+):pre=(\d+)", tok)
                    if mm and len(mm.groups()) == 5:
                        td, p, bw, lat, pre = mm.group(1), *map(int, mm.groups()[1:])
                        con.execute("INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
                                    "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?)",
                                  (td, p, p, bw, lat, pre))
                elif tok.startswith("vlan:"):
                    mm = _re2.match(r"vlan:([^:]+):(\d+)", tok)
                    if mm:
                        con.execute("INSERT OR REPLACE INTO vlan_groups(id,name,vlan_id) "
                                    "VALUES(?,?,?)", (mm.group(1), mm.group(1), int(mm.group(2))))
                elif tok.startswith("vmem:"):
                    mm = _re2.match(r"vmem:([^:]+):([^:]+)", tok)
                    if mm:
                        con.execute("INSERT OR IGNORE INTO vlan_members(group_id,device_id) "
                                    "VALUES(?,?)", (mm.group(1), mm.group(2)))
                elif tok.startswith("stream:"):
                    mm = _re2.match(r"stream:([^:]+):([^:]+):vlan=(\d+):prio=(\d+):lat=(\d+):iv=(\d+):st=(\d+)", tok)
                    if mm:
                        sid, talker = mm.group(1), mm.group(2)
                        vlan, prio, lat, iv, st = map(int, mm.groups()[2:])
                        con.execute("INSERT OR REPLACE INTO tsn_streams(stream_id,name,talker,vlan_id,"
                                   "max_latency_ns,max_interval_ns,priority,data_frame_prio,status,comment)"
                                   " VALUES(?,?,?,?,?,?,?,?,?,'')",
                                  (sid, sid, talker, vlan, lat, iv, prio, prio, st))
                elif tok.startswith("streammem:"):
                    mm = _re2.match(r"streammem:([^:]+):([^:]+)", tok)
                    if mm:
                        con.execute("INSERT OR IGNORE INTO tsn_stream_members(stream_id,role,device_id) "
                                    "VALUES(?,'listener',?)", (mm.group(1), mm.group(2)))
                elif tok.startswith("tas:"):
                    mm = _re2.match(r"tas:([^:]+):cycle=(\d+):tgt=(\S+)", tok)
                    if mm:
                        con.execute("INSERT OR REPLACE INTO tas_schedules(id,name,cycle_time_ns,"
                                   "deploy_target) VALUES(?,?,?,?)",
                                  (mm.group(1), mm.group(1), int(mm.group(2)), mm.group(3)))
                elif tok.startswith("gcl:"):
                    mm = _re2.match(r"gcl:([^:]+):(\d+):(\d+):(\d+)", tok)
                    if mm:
                        con.execute("INSERT OR REPLACE INTO gcl_entries(schedule_id,index,gate_state,"
                                   "duration_ns) VALUES(?,?,?,?)",
                                  (mm.group(1), int(mm.group(2)), int(mm.group(3)), int(mm.group(4))))
            con.commit()
            return {"ok": True, "msg": "rolled back to version " + str(vid)}
        if act == "create_version":
            name = body.get("name") or ("snapshot-%s" % time.strftime("%H%M%S"))
            device_id = body.get("device_id", "")
            payload = json.dumps({t: [dict(r) for r in con.execute("SELECT * FROM %s" % t)]
                                for t in ("devices", "qos_configs", "preemption_configs",
                                          "vlan_groups", "vlan_members", "tas_schedules",
                                          "gcl_entries", "timesync_status", "tsn_streams",
                                          "tsn_stream_members", "settings")}, sort_keys=True)
            con.execute("INSERT INTO config_versions(name,device_id,created_at,payload) VALUES(?,?,?,?)",
                      (name, device_id, int(time.time()), payload))
            con.commit()
            return {"ok": True, "msg": "config snapshot saved: " + name}
        if act == "list_versions":
            versions = [dict(r) for r in con.execute(
                "SELECT id,name,device_id,created_at FROM config_versions ORDER BY id DESC LIMIT 20")]
            return {"ok": True, "versions": versions}
        if act == "diff_versions":
            a = body.get("a"); b = body.get("b")
            if a is None and b is None:
                return {"ok": False, "msg": "select two versions"}
            latest = con.execute("SELECT MAX(id) FROM config_versions").fetchone()[0]
            if b is None:
                b = latest
            if a is None:
                a = latest
            pa = con.execute("SELECT payload FROM config_versions WHERE id=?", (int(a),)).fetchone()
            pb = con.execute("SELECT payload FROM config_versions WHERE id=?", (int(b),)).fetchone()
            if not pa or not pb:
                return {"ok": False, "msg": "version not found"}
            if pa[0] == pb[0]:
                return {"ok": True, "diff": "no differences"}
            # attempt a coarse field-level diff for JSON payloads
            try:
                da = json.loads(pa[0]); db_ = json.loads(pb[0])
            except Exception:
                da = db_ = None
            if isinstance(da, dict) and isinstance(db_, dict):
                lines = []
                for t in ("devices", "qos_configs", "vlan_groups", "tsn_streams"):
                    sa = json.dumps(da.get(t), sort_keys=True)
                    sb = json.dumps(db_.get(t), sort_keys=True)
                    if sa != sb:
                        lines.append(t)
                msg = "differs (v%d vs v%d): %s" % (int(a), int(b), ", ".join(lines))
                return {"ok": True, "diff": msg}
            return {"ok": True, "diff": "versions differ (v%d vs v%d)" % (int(a), int(b))}
        if act == "sync_report":
            r = body
            if r.get("device_id"):
                con.execute("INSERT INTO timesync_reports(device_id,ts,offset_ns,jitter_ns,"
                            "packet_count,packet_loss,status) VALUES(?,?,?,?,?,?,?)",
                            (r["device_id"], int(time.time()), int(r.get("offset_ns", 0)),
                             int(r.get("jitter_ns", 0)), int(r.get("packet_count", 0)),
                             int(r.get("packet_loss", 0)), r.get("status", "in_sync")))
                con.commit()
                return {"ok": True, "msg": "sync report recorded"}
            return {"ok": False, "msg": "missing device_id"}
        if act == "restore_backup":
            data = body.get("data", body)
            if not data or not isinstance(data, dict):
                return {"ok": False, "msg": "invalid backup payload"}
            tables = ["devices", "qos_configs", "preemption_configs", "vlan_groups",
                      "vlan_members", "tas_schedules", "gcl_entries", "timesync_status",
                      "tsn_streams", "tsn_stream_members", "settings"]
            allowed_indexes = {"gcl_entries": "schedule_id,index", "tsn_stream_members": "stream_id,role,device_id",
                              "vlan_members": "group_id,device_id", "vlan_groups": "id",
                              "tas_schedules": "id", "timesync_status": "id", "devices": "id"}
            try:
                for t in tables:
                    rows = data.get(t)
                    if rows is None:
                        continue
                    if not isinstance(rows, list):
                        rows = [rows]
                    con.execute("DELETE FROM %s" % t)
                    for r in rows:
                        if not isinstance(r, dict):
                            continue
                        cols = []
                        vals = []
                        for k, v in r.items():
                            if k == "meta":
                                continue
                            if _re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k) and isinstance(v, (str, int, float)):
                                cols.append("`" + k + "`")
                                vals.append(v)
                        if not cols:
                            continue
                        con.execute("INSERT INTO %s(%s) VALUES(%s)" %
                                   (t, ",".join(cols), ",".join(["?"] * len(vals))), tuple(vals))
                con.commit()
                return {"ok": True, "msg": "configuration restored"}
            except Exception as exn:
                return {"ok": False, "msg": "restore failed: " + str(exn)}
    except Exception as ex:
        return {"ok": False, "msg": str(ex)}
    finally:
        con.close()


WEB_HOST = os.environ.get("WTSN_HOST", "127.0.0.1")
WEB_USER = os.environ.get("WTSN_WEB_USER") or None
WEB_PASS = os.environ.get("WTSN_WEB_PASS") or ""
MAX_BODY = 1 << 20

def _basic_auth(user, pw):
    import base64
    return "Basic " + base64.b64encode(("%s:%s" % (user, pw)).encode("utf-8")).decode("ascii")


def make_handler():
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, b):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def _check_auth(self):
            if WEB_USER is None:
                return True
            return self.headers.get("Authorization") == _basic_auth(WEB_USER, WEB_PASS)

        def do_GET(self):
            if not self._check_auth():
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="WTSN Configurator"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            p = urlsplit(self.path).path
            if p == "/api/data":
                d = load_all()
                d["mode"] = MODE["mode"]
                with EVENT_LOCK:
                    d["events"] = list(EVENTS)[:300]
                self._send(json.dumps(d).encode())
            elif p == "/api/events":
                self._send(json.dumps(get_events()).encode())
            else:
                body = HTML.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        def do_POST(self):
            if not self._check_auth():
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="WTSN Configurator"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if urlsplit(self.path).path != "/api/action":
                self.send_error(404)
                return
            try:
                n = int(self.headers.get("Content-Length", 0))
                if n < 0 or n > MAX_BODY:
                    self.send_error(413)
                    return
                body = json.loads(self.rfile.read(n) or b"{}")
            except Exception:
                body = {}
            res = run_action(body.get("action"), body.get("data", body))
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            b = json.dumps(res).encode()
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

    return H


HTML = r"""<!doctype html><html><head><meta charset="utf-8">
<title>WTSN Configurator</title>
<style>
:root{--bg:#0e0e16;--surf:#161622;--surf2:#1e1e2c;--pri:#4C8DFF;--sec:#2f9ee6;--ok:#31C96B;--warn:#FFC94A;--err:#FF5F56;--text:#E8E8F0;--dim:#8a8aa0;--border:#2a2a3a}
*{box-sizing:border-box;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
body{margin:0;background:var(--bg);color:var(--text);height:100vh;display:flex;flex-direction:column;overflow:hidden}
header{display:flex;align-items:center;gap:14px;padding:0 16px;height:54px;background:var(--surf);border-bottom:1px solid var(--border);flex-shrink:0}
header .logo{font-weight:700;font-size:16px;color:var(--pri)}
header .sub{color:var(--dim);font-size:12px}
.execbtn{margin-left:16px;padding:8px 14px;background:var(--pri);color:#fff;border:none;border-radius:8px;cursor:pointer;font-size:13px;font-weight:600}
.execbtn:hover{filter:brightness(1.1)}
.spacer{flex:1}
.muted{color:var(--dim);font-size:13px}
.mode{display:flex;border:1px solid var(--border);border-radius:8px;overflow:hidden}
.mode button{padding:7px 14px;background:none;color:var(--dim);border:none;cursor:pointer;font-size:13px}
.mode button.on{background:var(--pri);color:#fff}
.wrap{display:flex;flex:1;overflow:hidden}
nav{width:200px;background:var(--surf);border-right:1px solid var(--border);padding:8px 0;overflow:auto;flex-shrink:0}
nav button{display:block;width:100%;padding:10px 14px;background:none;border:none;color:var(--dim);text-align:left;font-size:14px;cursor:pointer}
nav button.on{color:var(--text);background:var(--surf2);border-left:3px solid var(--pri)}
.navgroup{margin-bottom:4px}
.navtitle{color:var(--sec);font-size:11px;font-weight:700;letter-spacing:.04em;padding:10px 14px 4px;text-transform:uppercase}
main{flex:1;overflow:auto;padding:18px;padding-bottom:140px}
button.big{width:100%;font-size:14px;padding:11px;background:var(--sec);color:#04121c;font-weight:700}
label.ck{width:auto;display:inline-flex;align-items:center;gap:4px;margin:4px 8px 0 0;color:var(--text)}
label.ck input{width:auto}
h2{margin:0 0 12px;font-size:18px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px;margin-bottom:16px}
.stat{border:1px solid var(--border);border-radius:10px;padding:14px;background:var(--surf)}
.stat .l{color:var(--dim);font-size:12px}
.stat .v{font-size:26px;margin-top:4px;color:var(--ok)}
.card{border:1px solid var(--border);border-radius:10px;background:var(--surf);padding:14px;margin-bottom:14px}
.card h3{margin:0 0 10px;font-size:15px}
.row{display:flex;gap:8px;align-items:center;margin:6px 0;flex-wrap:wrap}
label{color:var(--dim);font-size:12px;width:120px;flex-shrink:0}
input,select{background:var(--surf2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:6px}
input[type=number]{width:100px}
button{background:var(--pri);color:#fff;border:none;border-radius:6px;padding:7px 12px;cursor:pointer}
button.danger{background:#8a0000}button.ghost{background:none;color:var(--dim);border:1px solid var(--border)}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{border-bottom:1px solid var(--border);padding:6px;text-align:left}th{color:var(--dim)}
#toast{position:fixed;bottom:18px;right:18px;background:var(--surf2);border:1px solid var(--sec);padding:10px 14px;border-radius:8px;display:none;z-index:50}
.monrow{display:flex;gap:10px;padding:4px 0;border-bottom:1px solid var(--border);font-family:ui-monospace,monospace;font-size:12px}
.monrow .t{color:var(--dim);width:70px}.monrow .s{color:var(--sec);width:110px}.monrow .ip{color:var(--dim);width:130px;font-family:ui-monospace,monospace}.monrow .pro{color:var(--ok);width:170px;font-weight:700}
.gclbox{background:var(--surf2);border:1px solid var(--border);border-radius:8px;padding:10px;margin-top:8px}
.gclrow{display:flex;align-items:center;gap:8px;margin:3px 0}
.gq{width:22px;color:var(--dim);font-size:11px;text-align:right;flex-shrink:0}
.gcllegend{color:var(--dim);font-size:11px;margin-top:8px;word-break:break-all}
.badge{display:inline-block;padding:2px 8px;border-radius:10px;font-size:12px;font-weight:600}
.badge.ok{color:#04190b;background:var(--ok)}.badge.warn{color:#241c00;background:var(--warn)}.badge.err{color:#fff;background:var(--err)}.badge.dim{color:var(--text);background:var(--border)}
code{background:var(--surf2);padding:1px 5px;border-radius:4px;font-family:ui-monospace,monospace;font-size:12px;color:var(--sec)}
ul{padding-left:20px}.card p{margin:6px 0}.card li{margin:3px 0}
</style></head><body>
<header><span class="logo">WTSN Configurator</span><span class="sub">Wireless TSN control plane</span>
<button class="execbtn" onclick="execAll()">Execute settings on controller</button>
<div class="spacer"></div>
<span class="mode"><button id="md_real" onclick="setMode('real')">Real</button><button id="md_sim" class="on" onclick="setMode('sim')">Simulation</button></span>
</header>
<div class="wrap"><nav id="nav"></nav><main id="main"></main></div>
<div id="toast"></div>
<script>
const NAV=[["System",[["devices","Devices"],["domains","Domains"],["versions","Config Versions"],["monitor","Monitor"],["sensors","Sensors"],["help","Help"]]],["OPC UA FX over MQTT",[["fxmqtt","FXMQTT Config"]]],["IEEE 802.1AS",[["timesync","Synchronization"]]],["IEEE 802.1Q",[["qos","QoS Priority"],["vlan","WVLAN ID"]]],["IEEE 802.1Qbv",[["tas","TAS / GCL"]]],["IEEE 802.1Qbu",[["preemption","Preemption"]]],["IEEE 802.1Qcc",[["streams","TSN Streams"]]]];
let D={},cur="devices",MON_RUNNING=true;
function $(id){return document.getElementById(id)}
function esc(s){return String(s==null?"":s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]))}
async function api(a,d){const r=await fetch("/api/action",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:a,data:d})});const j=await r.json();toast(j.msg,j.ok);return j}
function toast(m,ok){const t=$("toast");t.style.display="block";t.style.borderColor=ok?"var(--sec)":"var(--err)";t.textContent=m;clearTimeout(t._h);t._h=setTimeout(()=>t.style.display="none",2200)}
async function load(){const r=await fetch("/api/data");D=await r.json();renderNav();go(cur||"devices")}
function renderNav(){const n=$("nav");n.innerHTML="";
 NAV.forEach(g=>{if(g[0]){const h=document.createElement("div");h.className="navgroup";n.appendChild(h);const t=document.createElement("div");t.className="navtitle";t.textContent=g[0];h.appendChild(t)}const cont=g[1];cont.forEach(pp=>{const b=document.createElement("button");b.textContent=pp[1];b.onclick=()=>go(pp[0]);b.id="nv_"+pp[0];(g[0]?n.lastElementChild:n).appendChild(b)})});
 $("md_real").className=D.mode==="real"?"on":"";$("md_sim").className=D.mode==="sim"?"on":""}
function setMode(m){api("set_mode",{mode:m}).then(load)}
function stat(l,v){return "<div class='stat'><div class='l'>"+l+"</div><div class='v'>"+v+"</div></div>"}
function stBadge(s){const map=[["ok","online"],["warn","offline"],["err","error"]];const b=map[s]||["dim",s];return "<span class='badge "+(b[0])+"'>"+esc(b[1])+"</span>"}
function types(d){return (D.device_tsn_features||[]).filter(f=>f.device_id==d).map(f=>f.feature).join(", ")}
function tsnOpts(gm){return D.devices.map(d=>"<label><input type=checkbox value='"+esc(d.id)+"' "+(d.id==gm?"checked":"")+"> GM:"+esc(d.id)+"</label>").join("")}
function go(p){cur=p;document.querySelectorAll("#nav button").forEach(b=>b.className=b.id==="nv_"+p?"on":"");
 const m=$("main");
 if(p==="devices")m.innerHTML=devices();
 else if(p==="domains")m.innerHTML=domains();
 else if(p==="versions")m.innerHTML=versions();
 else if(p==="qos")m.innerHTML=qos();
 else if(p==="vlan")m.innerHTML=vlan();
 else if(p==="tas")m.innerHTML=tas();
 else if(p==="preemption")m.innerHTML=preemption(); else if(p==="timesync"){m.innerHTML=timesync();redrawSetup();}
 else if(p==="streams")m.innerHTML=streams();
 else if(p==="fxmqtt")m.innerHTML=fxmqtt();
 else if(p==="monitor"){m.innerHTML=monitor();refreshMon();}
 else if(p==="sensors")m.innerHTML=sensors();
 else if(p==="help")m.innerHTML=help();
 else if(p==="settings")m.innerHTML=settings();}
function execAll(){api("exec_all",{}).then(load)}
function devices(){
 const rows=D.devices.map(d=>"<tr><td>"+esc(d.id)+"</td><td>"+esc(d.name)+"</td><td>"+stBadge(d.status)+"</td><td>"+esc(d.ip)+"</td><td>"+esc(tsnFor(d.id))+"</td><td><button onclick='api(\"ping_device\",{id:\""+esc(d.id)+"\"})'>Ping</button></td><td><button class='danger' onclick='api(\"save_devices\",{delete:[\""+esc(d.id)+"\"]}).then(load)'>Del</button></td></tr>").join("");
 return "<h2>Devices</h2><div class='card'><h3>Add TSN device</h3>"+
  "<div class='row'><label>id</label><input id=did></div>"+
  "<div class='row'><label>name</label><input id=dname></div>"+
  "<div class='row'><label>ip</label><input id=dip></div>"+
  "<button onclick=saveDev()>Add Device</button></div>"+
  "<h3>Devices</h3><table><tr><th>id</th><th>name</th><th>status</th><th>ip</th><th>tsn</th><th></th><th></th></tr>"+rows+"</table>"}
function tsnFor(id){
 const f=(D.device_tsn_features||[]).filter(x=>x.device_id===id).map(x=>x.feature);
 if((D.qos_configs||[]).some(q=>q.device_id===id))f.push("802.1Q QoS");
 if((D.vlan_members||[]).some(m=>m.device_id===id)&&(D.vlan_groups||[]).length)f.push("802.1Q VLAN");
 if((D.tsn_stream_members||[]).some(m=>m.device_id===id))f.push("802.1Qcc Streams");
 if((D.preemption_configs||[]).some(p=>p.device_id===id&&p.preemption==1))f.push("802.1Qbu Preemption");
 if((D.tas_schedules||[]).some(t=>t.deploy_target===id))f.push("802.1Qbv TAS");
 return Array.from(new Set(f)).join(", ")}
function saveDev(){api("save_devices",{device:{id:$("did").value,name:$("dname").value,ip:$("dip").value,firmware:"",kind:0,status:0,tsn:[]}}).then(load)}
function domains(){const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 const dm=(D.domains||[]).map(g=>{const members=(D.devices||[]).filter(d=>d.domain===g.id).map(x=>x.id);
  return "<div class='card'><h3>"+esc(g.name)+" <span class='muted'>("+esc(g.id)+")</span><span class='muted' style='float:right'>"+members.length+" devices</span></h3>"+
   "<div class='row'><label>description</label><span>"+esc(g.description)+"</span></div>"+
   "<div class='row'><label>members</label><table><tr><th>device</th><th>domain</th></tr>"+members.map(m=>"<tr><td>"+esc(m)+"</td><td>"+esc(g.id)+"</td></tr>").join("")+"</table></div>"+
   (g.id!=="default"?"<button class='danger' onclick='api(\"delete_domain\",{id:\""+esc(g.id)+"\"}).then(load)'>Delete domain</button>":"")+"</div>"}).join("");
 return "<h2>TSN Domains</h2><div class='muted' style='margin-bottom:10px'>Physical 802.11 cells each form their own collision/time domain. Group devices by domain so QoS/VLAN/TAS configuration is scoped correctly.</div><div class='card'><h3>Add domain</h3><div class='row'><label>id</label><input id=nv_id placeholder='e.g. shopfloor'></div><div class='row'><label>name</label><input id=nv_name placeholder='Shop Floor'></div><div class='row'><label>description</label><input id=nv_desc></div><button onclick='api(\"save_domain\",{id:$(\"nv_id\").value,name:$(\"nv_name\").value,description:$(\"nv_desc\").value}).then(load)'>Add domain</button></div><div class='card'><h3>Assign device to domain</h3><div class='row'><label>device</label><select id=vd_dev>"+opts+"</select></div><div class='row'><label>domain</label><select id=vd_dom>"+(D.domains||[]).map(x=>"<option value='"+esc(x.id)+"'>"+esc(x.name)+"</option>").join("")+"</select></div><button onclick='assignDom()'>Assign</button></div>"+dm}
function assignDom(){api("assign_domain",{device_id:$("vd_dev").value,domain:$("vd_dom").value}).then(load)}
function versions(){
 return "<h2>Config Versions / Rollback</h2><div class='card'><div class='row'><span class='muted'>Snapshot the current configuration so you can diff and roll back after a deploy.</span></div><div class='row'><label>name</label><input id=cv_name placeholder='pre-deploy'></div><button onclick='api(\"create_version\",{name:$(\"cv_name\").value}).then(load)'>Save snapshot</button> <button class='ghost' onclick='rvLoad()'>List versions</button></div><div id=cv_list></div>"}
function rvLoad(){const m=$("cv_list");m.innerHTML="<div class='muted'>loading...</div>";fetch("/api/action",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:"list_versions"})}).then(r=>r.json()).then(j=>{if(!j.ok){m.innerHTML="<div class='muted'>"+esc(j.msg)+"</div>";return}const vl=(j.versions||[]).map(v=>"<div class='card'><div class='row'><span>"+esc(v.name)+"</span><span class='muted'>v"+v.id+" · "+new Date(v.created_at*1000).toLocaleString()+" · "+(v.device_id||"global")+"</span></div><div class='row'><button class='danger' onclick='rollback("+v.id+")'>Rollback</button> <button class='ghost' onclick='diffV("+v.id+")'>Diff vs latest</button></div></div>").join("");m.innerHTML=vl||"<div class='muted'>no versions yet</div>"})}
function rollback(id){if(!confirm("Roll back configuration to this version?"))return;api("rollback_version",{id:id}).then(load)}
function diffV(id){fetch("/api/action",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:"diff_versions",a:id})}).then(r=>r.json()).then(j=>toast(j.diff||j.msg,j.ok))}

function devices_render(){const hdr=[].slice.call(document.querySelectorAll("h3")).find(h=>h.textContent==="Devices");const tbl=hdr?hdr.nextElementSibling:null;if(tbl&&tbl.tagName==="TABLE"){const rows=D.devices.map(d=>"<tr><td>"+esc(d.id)+"</td><td>"+esc(d.name)+"</td><td>"+stBadge(d.status)+"</td><td>"+esc(d.ip)+"</td><td>"+esc(tsnFor(d.id))+"</td><td><button onclick='api(\"ping_device\",{id:\""+esc(d.id)+"\"})'>Ping</button></td><td><button class='danger' onclick='api(\"save_devices\",{delete:[\""+esc(d.id)+"\"]}).then(load)'>Del</button></td></tr>").join("");tbl.innerHTML="<tr><th>id</th><th>name</th><th>status</th><th>ip</th><th>tsn</th><th></th><th></th></tr>"+rows}}
const PRIOS=[[0,"Background (background data)"],[1,"Best effort"],[2,"Excellent effort"],[3,"Critical application"],[4,"Video (latency and jitter below 100 ms)"],[5,"Voice (latency and jitter below 10 ms)"],[6,"Internetwork control (network control)"],[7,"Control data traffic (data traffic control)"]];
function qos(){const def=(D.qos_configs[0]||{}).priority;
 let rows=D.qos_configs.map(q=>"<tr><td>"+esc(q.device_id)+"</td><td>Priority "+q.priority+" — "+(PRIOS.find(p=>p[0]==q.priority)||["",q.priority])[1]+"</td><td>"+q.latency_ms+" ms</td><td><button class='danger' onclick='api(\"delete_qos\",{device_id:\""+esc(q.device_id)+"\"}).then(load)'>Clear</button></td></tr>").join("");
 const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 const prio=PRIOS.map(p=>"<option value='"+p[0]+"' "+(String(def)==String(p[0])?"selected":"")+">Priority "+p[0]+" — "+esc(p[1])+"</option>").join("");
 return "<h2>IEEE 802.1Q — QoS Priority</h2><div class='card'>"+
  "<div class='row'><label>device</label><select id=q_dev><option value=''>-</option>"+opts+"</select></div>"+
  "<div class='row'><label>priority</label><select id=q_prio>"+prio+"</select></div>"+
  "<div class='row'><label>latency ms</label><input id=q_lat type=number value=2></div>"+
  "<button onclick=saveQ()>Save QoS</button></div>"+
  "<h3>Current settings</h3>"+
  "<table><tr><th>device</th><th>priority</th><th>latency ms</th><th></th></tr>"+rows+"</table>"}
function saveQ(){const d=$("q_dev").value;const p=parseInt($("q_prio").value);api("save_qos",{device_id:d,priority:p,traffic_class:p,latency_ms:parseInt($("q_lat").value),preemption:(D.qos_configs.find(q=>q.device_id==d)||{}).preemption||0}).then(load)}
function vlan(){let cards=D.vlan_groups.map(g=>{const members=(D.vlan_members||[]).filter(x=>x.group_id==g.id).map(x=>x.device_id);
  const all=D.devices.map(d=>"<option value='"+esc(d.id)+"' "+(members.includes(d.id)?"selected":"")+">"+esc(d.id)+"</option>").join("");
  return "<div class='card' style='width:100%' data-gid='"+esc(g.id)+"'><h3>"+esc(g.name)+" (vlan ID "+g.vlan_id+")</h3>"+
   "<table><tr><th>device</th><th>role</th></tr>"+members.map(m=>"<tr><td>"+esc(m)+"</td><td>member</td></tr>").join("")+"</table>"+
   "<div class='row'><label>member(s)</label><select multiple size=6>"+all+"</select></div>"+
   "<div class='row'><span class=muted>Ctrl+click to select multiple, then Apply</span></div>"+
   "<button onclick='vlanApply(this)'>Apply members</button> <button class='danger' onclick='api(\"delete_vlan\",{id:\""+esc(g.id)+"\"}).then(load)'>Clear group</button></div>"}).join("");
 return "<h2>WVLAN ID (IEEE 802.1Q)</h2><div class='card'><h3>Add WVLAN group</h3><div class='row'><label>name</label><input id=v_name></div><div class='row'><label>WVLAN ID 1-4094</label><input id=v_id type=number value=100></div><button onclick='api(\"save_vlan\",{id:\"v_\"+Math.random().toString(36).slice(2,6),name:$(\"v_name\").value,vlan_id:parseInt($(\"v_id\").value)}).then(load)'>Add group</button></div>"+cards}
function vlanApply(btn){const card=btn.closest(".card");const sel=card.querySelector("select");const picked=[].slice.call(sel.options).filter(o=>o.selected).map(o=>o.value);api("save_member",{group_id:card.dataset.gid,set_members:picked}).then(load)}
function gclBlock(s){const entries=(D.gcl_entries||[]).filter(e=>e.schedule_id==s.id);
 if(!entries.length)return "<div class='muted'>no GCL entries</div>";
 const cycle=Math.max(1,s.cycle_time_ns||1);
 const total=entries.reduce((a,e)=>a+(e.duration_ns||0),0);
 const scale=Math.min(900,total?total:1)/Math.max(1,entries.length);
 let bars="";let acc=0;
 entries.forEach((e,i)=>{const w=e.duration_ns?e.duration_ns*Math.min(600,total)/Math.max(1,total):20;
  const col=e.gate_state?"var(--ok)":"var(--err)";
  bars+="<div class='gclrow'><span class='gq'>Q"+(Math.log2(e.gate_state||0)|0)+"</span>"+
   "<div style='background:"+col+";height:16px;width:"+Math.max(8,w)+"px;border-radius:4px' title='gate "+e.gate_state+" · "+e.duration_ns+" ns'></div>"+
   "<span class='gcllegend'>"+e.duration_ns+" ns</span></div>";});
 const legend=entries.map((e,i)=>"Q"+(Math.log2(e.gate_state||0)|0)+"="+esc(e.duration_ns)+"ns").join(" · ");
 return "<div class='gclbox'><div style='font-size:12px;color:var(--dim);margin-bottom:6px'>Gate Control List — "+cycle+" ns cycle</div>"+bars+"<div class='gcllegend'>"+legend+"</div></div>"}
function tas(){let bl=D.tas_schedules.map(s=>{const g=(D.gcl_entries||[]).filter(e=>e.schedule_id==s.id).map(e=>e.gate_state+":"+e.duration_ns).join(", ");const repr=gclBlock(s);return "<div class='card'><h3>"+esc(s.name)+"</h3><div class='row'><label>id</label><span>"+esc(s.id)+"</span></div><div class='row'><label>cycle</label><span>"+s.cycle_time_ns+" ns</span></div><div class='row'><label>deploy</label><span>"+esc(s.deploy_target)+"</span></div>"+repr+"<div class='row'><label>GCL</label><span>"+esc(g)+"</span></div><button class='danger' onclick='api(\"delete_tas\",{id:\""+esc(s.id)+"\"}).then(load)'>Clear</button></div>"}).join("");
 const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 return "<h2>TAS (IEEE 802.1Qbv) — Gate Control List</h2><div class='card'><div class='row'><label>name</label><input id=t_name></div><div class='row'><label>cycle ns</label><input id=t_cyc type=number value=1000000></div><div class='row'><label>deploy target</label><select id=t_dev>"+opts+"</select></div><div class='row'><label>GCL</label><input id=t_gcl value='1:300000,3:200000,0:500000'></div><button onclick=tSave()>Save / Deploy</button></div>"+bl}
function tSave(){const g=$("t_gcl").value.split(",").map(x=>{const[a,b]=x.split(":");return{gate_state:parseInt(a),duration_ns:parseInt(b)}});api("save_tas",{id:"s",name:$("t_name").value||"schedule",cycle_time_ns:parseInt($("t_cyc").value),deploy_target:$("t_dev").value,gcl:g}).then(load)}
function preemption(){
 const rows=(D.preemption_configs||[]).map(p=>"<tr><td>"+esc(p.device_id)+"</td><td>"+(["off","on","on"][p.preemption]||p.preemption)+"</td><td>"+esc(p.emac)+"</td><td>"+esc(p.pmac)+"</td><td><button class='danger' onclick='api(\"delete_preemption\",{device_id:\""+esc(p.device_id)+"\"}).then(load)'>Clear</button></td></tr>").join("");
 const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 return "<h2>IEEE 802.1Qbu — Frame Preemption</h2>"+
  "<div class='card'><div class='row'><label>device</label><select id=pr_dev><option value=''>-</option>"+opts+"</select></div>"+
  "<div class='row'><label>mode</label><select id=pr_mode onchange=prMode()><option value=0>off (no preemption)</option><option value=1>on</option></select></div>"+
  "<div id=pr_mac style='display:none'>"+
  "<div class='row'><label>eMAC priorities</label><input id=pr_emac placeholder='e.g. 7,6,5'></div>"+
  "<div class='row'><label>pMAC priorities</label><input id=pr_pmac placeholder='e.g. 3,2,1,0'></div></div>"+
  "<button onclick=savePre()>Save Preemption</button></div>"+
  "<h3>Current settings</h3><table><tr><th>device</th><th>mode</th><th>eMAC prios</th><th>pMAC prios</th><th></th></tr>"+rows+"</table>"}
function prMode(){const on=$("pr_mode").value=="1";$("pr_mac").style.display=on?"block":"none"}
function savePre(){const d=$("pr_dev").value;if(!d)return;api("save_preemption",{device_id:d,preemption:parseInt($("pr_mode").value),emac:$("pr_emac").value,pmac:$("pr_pmac").value}).then(load)}
function timesync(){const t=(D.timesync_status||[])[0]||{};
 const gm=t.grandmaster||"PC";
 const slaveNodes=(D.settings||[]).find(s=>s.key==="sync_nodes")||{};
 const chosen=(slaveNodes.value||"").split(",").filter(Boolean);
 const nodes=D.devices.map(d=>"<option value='"+esc(d.id)+"' "+(chosen.includes(d.id)?"selected":"")+">"+esc(d.id)+" (ID "+esc(nodeId(d))+")</option>").join("");
 const slaveRows=(chosen.length?chosen:[]).map(n=>"<tr><td>Slave</td><td>"+esc(n)+"</td><td>"+esc(nodeId({id:n}))+"</td><td><button class='ghost' onclick=delSlave(\""+esc(n)+"\")>Remove</button></td></tr>").join("");
 const gmSel="<select id=ts_gm onchange=gmChanged()><option value='PC' "+(gm==='PC'?'selected':'')+">PC (configurator)</option>"+
   D.devices.map(d=>"<option value='"+esc(d.id)+"' "+(gm===d.id?'selected':'')+">"+esc(d.id)+"</option>").join("")+"</select>";
 const usePc=gm==='PC';
 const setupTable=("<div class='card'><table><tr><th>role</th><th>node</th><th>identity</th><th></th></tr>"+
  "<tr><td>Master</td><td>"+(usePc?"PC (configurator)":esc(gm))+"</td><td>"+esc(nodeId(usePc?{id:"PC"}:{id:gm}))+"</td><td>"+(usePc?"":"<button class='dangergm' onclick='delGM()'>Clear</button>")+"</td></tr>"+
  slaveRows+"</table></div>");
 return "<h2>IEEE 802.1AS — gPTP Time Synchronization</h2>"+
  "<div class='card'><h3>Grandmaster (Sync Master)</h3><div class='row'><label>GM</label>"+gmSel+"</div>"+
  "<div class='row'><label>identity</label><span id=gm_id>"+esc(nodeId(usePc?{id:"PC"}:{id:gm}))+"</span></div></div>"+
  "<div class='card'><h3>Sync Slave Nodes (follow the GM)</h3><select id=ts_slaves multiple size=6>"+nodes+"</select>"+
  "<div class='row'><span class=muted>Ctrl+click to select multiple</span></div></div>"+
  "<div class='card'><button class=big onclick=saveTimeSync()>Save 802.1AS Sync</button></div>"+
  "<h3>Current setup</h3><div id=setupDraw>"+setupTable+"</div>"}
function gmChanged(){const g=$("ts_gm").value;const isPC=g==="PC";$("gm_id").textContent=nodeId(isPC?{id:"PC"}:{id:g});
 // if an ESP device is chosen as GM, keep it highlighted but don't save until Save pressed
 api("save_timesync",{mode:1,grandmaster:g,nodes:[]}).then(()=>{setSyncNodesLocal([]);redrawSetup()})}
function nodeData(id){const dv=(D.devices||[]).find(d=>d.id===id);return dv||{id:id}}
function syncNodeId(id){const dv=nodeData(id);return (dv.mac||"").replace(/:|\./g,"")||("0000"+String(id||"").split("").map(c=>(c.charCodeAt(0)).toString(16)).join("")).slice(-16)}
function nodeId(d){const id=(d&&d.id)||"PC";if(id==="PC")return "80:00:11:ff:fe:00:00:01";const dv=nodeData(id);if(dv&&dv.mac){const m=dv.mac.split(":").filter(Boolean);if(m.length===4)return "80:00:11:ff:fe:"+m.map(x=>x.padStart(2,"0")).join(":")}return "80:00:11:ff:fe:"+[0,0,2,3].map(()=>Math.floor(Math.random()*256).toString(16).padStart(2,"0")).join(":")}
function redrawSetup(){const el=$("setupDraw");if(!el)return;const ts=(D.timesync_status||[])[0]||{};const gm=ts.grandmaster||"PC";
 const slaveNodes=(D.settings||[]).find(s=>s.key==="sync_nodes")||{};const chosen=(slaveNodes.value||"").split(",").filter(Boolean);
 const isPC=gm==="PC";const rows=chosen.map(n=>"<tr><td>Slave</td><td>"+esc(n)+"</td><td>"+esc(nodeId({id:n}))+"</td><td><button class='ghost' onclick=delSlave(\""+esc(n)+"\")>Remove</button></td></tr>").join("");
 const setupTable="<table><tr><th>role</th><th>node</th><th>identity</th><th></th></tr><tr><td>Master</td><td>"+(isPC?"PC (configurator)":esc(gm))+"</td><td>"+esc(nodeId(isPC?{id:"PC"}:{id:gm}))+"</td><td>"+(isPC?"":"<button class='dangergm' onclick='delGM()'>Clear</button>")+"</td></tr>"+rows+"</table>";
 el.innerHTML=setupTable}
function setSyncNodesLocal(nodes){let sv=(D.settings||[]).find(s=>s.key==="sync_nodes");if(!sv){if(!D.settings)return;sv={key:"sync_nodes",value:""};D.settings.push(sv)}sv.value=(nodes||[]).join(",")}
function gmNodeList(){return (D.timesync_status||[])[0]?D.timesync_status[0].grandmaster||"PC":"PC"}
function delGM(){api("save_timesync",{mode:1,grandmaster:"PC",nodes:[]}).then(()=>{setSyncNodesLocal([]);redrawSetup()})}
function delSlave(n){const cur=(D.settings||[]).find(s=>s.key==="sync_nodes")||{};
 const rest=(cur.value||"").split(",").filter(Boolean).filter(x=>x!==n);
 api("save_timesync",{mode:1,grandmaster:(D.timesync_status||[])[0]?D.timesync_status[0].grandmaster||"PC":"PC",nodes:rest}).then(()=>{setSyncNodesLocal(rest);redrawSetup()})}
function saveTimeSync(){const n=[].slice.call(document.querySelectorAll("#ts_slaves option:checked")).map(o=>o.value);
 const g=$("ts_gm").value;
 api("save_timesync",{grandmaster:g,offset_ns:(D.timesync_status||[])[0]?D.timesync_status[0].offset_ns||0:0,quality:0,mode:1,nodes:n}).then(()=>{setSyncNodesLocal(n);redrawSetup()})}
function fxmqtt(){
 const srv=(D.settings||[]).find(s=>s.key==="server_type")||{};
 const srvid=(D.settings||[]).find(s=>s.key==="server_id")||{};
 const brk=(D.settings||[]).find(s=>s.key==="broker")||{};
 const isPc=(srv.value||"pc")==="pc";
 const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"' "+(srvid.value==d.id?"selected":"")+">"+esc(d.id)+"</option>").join("");
 return "<h2>IEEE 802.1FX — FXMQTT Config</h2><div class='card'><h3>Field Server / Participant</h3>"+
  "<div class='row'><label>server</label><select id=fs_type onchange=fsMode()><option value='pc' "+(isPc?"selected":"")+">PC (configurator)</option><option value='node' "+(isPc?"":"selected")+">Node (device)</option></select></div>"+
  "<div class='row' id=fs_node_row><label>node</label><select id=fs_node "+(isPc?"disabled":"")+">"+opts+"</select></div>"+
  "<div class='row'><label>broker</label><input id=fs_broker placeholder='host:port' value='"+esc(brk.value||"127.0.0.1:1883")+"'></div>"+
  "<button onclick=fsSave()>Save / Deploy</button></div>"}
function fsMode(){const pc=$("fs_type").value==="pc";$("fs_node").disabled=pc;if(pc)$("fs_node").value=""}
function fsSave(){const type=$("fs_type").value;api("set_server",{type:type,id:(type==="node"?$("fs_node").value:""),broker:$("fs_broker").value||"127.0.0.1:1883"}).then(load)}
function monitor(){return "<h2>Network / Frame Monitor</h2><div class='card'><div class='row'><span>"+(D.mode==="sim"?"Live simulated flow (MQTT, FX over MQTT, raw frames)":"Real traffic — waiting for real nodes")+"</span><input id=mon_filter placeholder='filter...' style='width:160px;margin-left:10px' oninput='monFilter()'>"+"<span class='spacer'></span><button class='ghost' onclick='api(\"clear_events\",{}).then(load)'>Clear</button><button id=mon_toggle class='ghost' onclick='toggleMon()'>"+(MON_RUNNING?"Pause":"Start")+"</button><span id=mon_state class='muted'></span></div><div class='monrow'><span class='t'>time</span><span class='s'>source</span><span class='ip'>src IP</span><span class='s'>destination</span><span class='ip'>dst IP</span><span class='pro'>protocol</span><span>message</span></div><div id=mon></div></div>"}
function toggleMon(){MON_RUNNING=!MON_RUNNING;const b=$("mon_toggle");if(b){b.textContent=MON_RUNNING?"Pause":"Start"}const s=$("mon_state");if(s){s.textContent=MON_RUNNING?"":"PAUSED — new frames are kept and shown on Start"};refreshMon()}
function monFilter(){refreshMon()}
function refreshMon(){if($("mon")){const keep=$("mon").innerHTML;const going=MON_RUNNING||cur!=="monitor";const src=D.events||[];if(!going){$("mon").innerHTML=keep;if($("mon_state")&&$("mon_state").textContent.indexOf("beeen")<0)$("mon_state").textContent="PAUSED — new frames are kept and shown on Start";return}
  const f=($("mon_filter")?$("mon_filter").value:"").trim().toLowerCase();
  let list=src.slice(0,120);if(f){list=list.filter(e=>((e.source||"")+(e.dest||"")+(e.proto||"")+(e.msg||"")+(e.ts||"")).toLowerCase().indexOf(f)>=0)}
  const ev=list.map(e=>"<div class='monrow'><span class='t'>"+esc(e.ts)+"</span><span class='s'>"+esc(e.source)+"</span><span class='ip'>"+esc(e.src_ip||"-")+"</span><span class='s'>"+esc(e.dest||"-")+"</span><span class='ip'>"+esc(e.dst_ip||"-")+"</span><span class='pro'>"+esc(e.proto||"-")+"</span><span>"+esc(e.msg||e.data||"")+"</span></div>").join("");$("mon").innerHTML=ev||"<div class='monrow'><span>no traffic yet</span></div>"}}
function sensors(){
  const byDev={};(D.sensors||[]).forEach(s=>{const g=(byDev[s.device_id]=byDev[s.device_id]||{});g[s.sensor_id]=s});
  const now=Math.floor(Date.now()/1000);
  function age(s){if(!s||!s.last_update)return"";const a=now-(s.last_update>1000000000000?s.last_update/1000:s.last_update);return a<0?"":a<60?"updated "+Math.round(a)+"s ago":a<3600?"updated "+Math.floor(a/60)+"m ago":"updated "+Math.floor(a/3600)+"h "+Math.floor((a%3600)/60)+"m ago"}
  function stat(s){const a=s?now-(s.last_update>1000000000000?s.last_update/1000:s.last_update):9999;return a<30?"<span class='ok'>updated</span>":"<span class='warn'>stale</span>"}
  let r="";
  for(const dev in byDev){
    const g=byDev[dev],t=g.temp1,h=g.hum1,p=g.press1;
    if(!(t||h||p))continue;
    const any=t||h||p;
    if(t||h||p){const parts=[];if(h)parts.push("hum "+h.value+" %");if(t)parts.push("temp "+t.value+" °C");if(p)parts.push("press "+p.value+" hPa");r+="<tr><td>"+esc(dev)+"</td><td>BME280</td><td>"+esc(parts.join(" • "))+"</td><td>"+age(any)+"</td><td>"+stat(any)+"</td></tr>"}
    if(g.light1){r+="<tr><td>"+esc(dev)+"</td><td>Light</td><td>"+Math.round(g.light1.value)+" lx</td><td>"+age(g.light1)+"</td><td>"+stat(g.light1)+"</td></tr>"}
    if(g.pir1){const d=g.pir1.value?1:0;r+="<tr><td>"+esc(dev)+"</td><td>Motion</td><td>"+(d?"motion detected":"no motion")+"</td><td>"+age(g.pir1)+"</td><td class='"+(d?"det":"ok")+"'>"+(d?"detected":"")+"</td></tr>"}
  }
  return "<h2>Sensors ("+(D.mode==="real"?"real":"simulated")+")</h2>"+(r?("<table><tr><th>device</th><th>sensor</th><th>value</th><th>last updated</th><th>status</th></tr>"+r+"</table>"):"<div class='card'>"+(D.mode==="real"?"Connect real sensors — none identified yet.":"Waiting for simulation sensors...")+"</div>")}
function streams(){
  const rows=(D.tsn_streams||[]).map(s=>{const memb=(D.tsn_stream_members||[]).filter(x=>x.stream_id==s.stream_id);
    const talker=(memb.find(x=>x.role==="talker")||{}).device_id||s.talker||"";
    const lsn=memb.filter(x=>x.role==="listener").map(x=>x.device_id).join(", ");
    return "<tr><td>"+esc(s.stream_id)+"</td><td>"+esc(s.name)+"</td><td>"+esc(talker)+"</td><td>"+esc(lsn)+"</td><td>"+(s.vlan_id||0)+"</td><td>"+(["configured","ready","failed","standby"][s.status]||s.status)+"</td>"+
    "<td><button class='ghost' onclick='api(\"deploy_stream\",{stream_id:\""+esc(s.stream_id)+"\"}).then(load)'>Deploy</button>"+
    "<button class='danger' onclick='api(\"delete_stream\",{stream_id:\""+esc(s.stream_id)+"\"}).then(load)'>Del</button></td></tr>"
  }).join("");
  const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
  const vopts=D.vlan_groups.map(g=>"<option value='"+g.vlan_id+"'>"+esc(g.name)+" (VLAN "+g.vlan_id+")</option>").join("");
  return "<h2>IEEE 802.1Qcc — TSN Streams</h2>"+
   "<div class='card'><h3>Talker / Listener stream reservation</h3>"+
   "<div class='row'><label>name</label><input id=s_name></div>"+
   "<div class='row'><label>talker</label><select id=s_talker><option value=''>-</option>"+opts+"</select></div>"+
   "<div class='row'><label>listeners</label><select id=s_lsn multiple size=5>"+opts+"</select><span class='muted'>Ctrl+click to select multiple</span></div>"+
   "<div class='row'><label>vlan</label><select id=s_vlan><option value='0'>- none -</option>"+vopts+"</select></div>"+
   "<div class='row'><button onclick=saveStream()>Save Stream</button>"+
   "<button class='ghost' onclick='api(\"deploy_all_streams\",{}).then(load)'>Deploy All</button></div></div>"+
   "<h3>Streams</h3><table><tr><th>id</th><th>name</th><th>talker</th><th>listeners</th><th>vlan</th><th>status</th><th></th></tr>"+rows+"</table>"}
function saveStream(){const lsn=[].slice.call(document.querySelectorAll("#s_lsn option:checked")).map(o=>o.value);
 api("save_stream",{name:$("s_name").value,talker:$("s_talker").value,listeners:lsn,
  vlan_id:parseInt($("s_vlan")?$("s_vlan").value:0)||0,max_latency_ns:parseInt($("s_lat")?$("s_lat").value:1000000)||1000000,
  max_interval_ns:parseInt($("s_itv")?$("s_itv").value:100000)||100000,
  priority:parseInt($("s_prio")?$("s_prio").value:5)||5,data_frame_prio:parseInt($("s_prio")?$("s_prio").value:5)||5}).then(load)}
function step(i,t){return "<div class='card'><h3>Step "+i+". "+t+"</h3>"}
function help(){
 return "<h2>Help — How to configure this W-TSN network</h2>"+
  "<div class='card'><h3>Quick summary</h3><p>Click through in this order: <b>Devices → QoS → VLAN → TAS → (Preemption / TimeSync / Streams) → then the blue <i>Execute settings on controller</i> button in the header.</b></p>"+
  "<p>First, set your MQTT broker on the <b>FXMQTT Config</b> page (see step 3). It builds a JSON snapshot per device and publishes it on <code>tsn/cmd/&lt;id&gt;/apply</code> so every agent applies QoS / VLAN / TAS / TimeSync / streams. Watch the ACKs on the <b>Monitor</b> page.</p></div>"+
 step(1,"Start in Simulation mode")+"<p>The GUI starts in <b>Simulation</b> (top-right corner). This lets you try everything without hardware — nodes and sensors are simulated. Use it to learn the workflow. For real devices switch to <b>Real</b>.</p></div>"+
 step(2,"Set the MQTT broker / FXMQTT (one-time)")+"<p><b>OPC UA FX over MQTT → FXMQTT Config</b> (or Settings):</p>"+
 "<ul><li><b>Server</b> — is the Field Server the PC (configurator) or a node? Keep <b>PC (configurator)</b>.</li>"+
 "<li><b>Node</b> — only if a device acts as the server.</li>"+
 "<li><b>Broker</b> — host:port of your MQTT broker (e.g. <code>192.168.0.149:1883</code> or <code>wtsn-broker.local:1883</code>).</li></ul>"+
 "<p>Save / Deploy. FXMQTT is OPC UA FX / C2C Field Exchange carried over MQTT — this is the channel the configurator uses to talk to devices.</p></div>"+
 step(3,"Add your device")+"<p>Go to <b>Devices</b> and click the add form: enter an <b>id</b> (e.g. <code>esp32-01</code>) and a name. In Real mode the node itself provisions and announces over MQTT, so it usually appears automatically.</p>"+
 "<p>The <b>status badge</b> shows online / offline / error.</p>"+
 "<ul><li><b>Online</b> (green) — ready to configure.</li><li><b>Offline</b> (yellow) — device is not reporting; check WiFi / broker.</li><li><b>Error</b> (red) — something failed.</li></ul>"+
 "Use <b>Ping</b> to verify reachability.</div>"+
 step(4,"Set QoS priority")+"<p><b>IEEE 802.1Q → QoS Priority</b>: pick the device and the traffic priority (0–7). Higher priority = time-critical traffic like voice/control data. Set a latency budget in ms.</p>"+
 "<p>Save — then the priority appears in the table.</p></div>"+
 step(5,"Assign a VLAN")+"<p><b>IEEE 802.1Q → WVLAN ID</b>: create a group (name + VLAN ID 1–4094). Then, on that group card, select the member devices with Ctrl+click and press <b>Apply members</b>.</p>"+
 "<p>This tells which devices carry that VLAN tag.</p></div>"+
 step(6,"Schedule TAS / GCL")+"<p><b>IEEE 802.1Qbv → TAS / GCL</b>: enter a name, a cycle time in ns, the deploy target and a gate list such as <code>1:300000,3:200000,0:500000</code>.</p>"+
 "<p>The gate value is a bitmask of open queues (e.g. 1 = queue 0 only, 3 = queues 0+1, 0 = all closed). The next digit is the duration in ns. The visualization shows each gate window as a colored bar.</p>"+
 "<p>Save / Deploy.</p></div>"+
 step(7,"Optional: Preemption")+"<p><b>IEEE 802.1Qbu → Preemption</b>: to protect time-critical frames, turn preemption <b>on</b> for a device and define the express (eMAC) vs preemptable (pMAC) priority sets (e.g. eMAC <code>7,6,5</code>, pMAC <code>3,2,1,0</code>).</p></div>"+
 step(8,"Optional: Time sync")+"<p><b>IEEE 802.1AS → Synchronization</b>: choose the grandmaster and the slave nodes that follow it, then <b>Save 802.1AS Sync</b>.</p></div>"+
 step(9,"(Optional) TSN Streams")+"<p><b>IEEE 802.1Qcc → TSN Streams</b>: reserve a stream from a talker to listeners on a VLAN. Pick the talker, listeners (Ctrl+click), VLAN and Save.</p>"+
 "<p>Deploy it (or Deploy All) to push the reservation out.</p></div>"+
 step(10,"Apply everything — Execute settings on controller")+"<p>Once your devices and policies are set, click the blue <b>Execute settings on controller</b> button in the header.</p>"+
 "<p>This builds one JSON snapshot per device and publishes it on <code>tsn/cmd/&lt;id&gt;/apply</code> so every agent applies QoS / VLAN / TAS / TimeSync / streams. Watch the ACKs on the <b>Monitor</b> page.</p>"+
  "<ul><li>If MQTT broker is unreachable you will get an error toast.</li><li>Simulation just records the flow.</li></ul></div>"}
function settings(){
 const brk=(D.settings||[]).find(s=>s.key==="broker")||{};
 return "<h2>Settings</h2><div class='card'><div class='row'><label>mode</label><span>"+D.mode+"</span></div>"+
  "<div class='row'><label>MQTT broker</label><input id=broker_in value='"+esc(brk.value||"127.0.0.1:1883")+"'></div>"+
  "<button onclick=saveBroker()>Apply broker</button></div>"+
  "<div class='card'><div class='row'><label>db</label><span>"+esc(D.db||"wtsn_gui.db")+"</span></div>"+
  "<div class='row'><label>push</label><span>in <b>Real</b> mode /apply is sent over MQTT to devices</span></div></div>"+
  "<div class='card'><div class='row'><span class='muted'>Backup the whole configuration to JSON, or restore it from a previous export.</span></div>"+
  "<div class='row'><button onclick='exportConfig()'>Export config</button>"+
  "<button class='ghost' onclick='$(\"imp_file\").click()'>Restore from file</button>"+
  "<input type=file id=imp_file accept='.json,application/json' style='display:none' onchange='importConfig(this)'></div></div>"}
function saveBroker(){api("set_server",{type:((D.settings||[]).find(s=>s.key==="server_type")||{}).value||"node",id:"",broker:$("broker_in").value}).then(load)}
function dl(name,content,type){const b=new Blob([content],{type:type||"application/json"});if(window.navigator&&navigator.msSaveBlob)return navigator.msSaveBlob(b,name);const u=URL.createObjectURL(b);const a=document.createElement("a");a.href=u;a.download=name;document.body.appendChild(a);a.click();document.body.removeChild(a);setTimeout(()=>URL.revokeObjectURL(u),1000)}
function exportConfig(){const config=["devices","qos_configs","preemption_configs","vlan_groups","vlan_members","tas_schedules","gcl_entries","timesync_status","tsn_streams","tsn_stream_members","settings"].reduce((o,t)=>{o[t]=(D[t]||[]).map(r=>({...r}));return o},{meta:{mode:D.mode,exported:new Date().toISOString()}});dl("wtsn-config-"+D.mode+"-"+new Date().toISOString().slice(0,19).replace(/[:T]/g,"-")+".json",JSON.stringify(config,null,2));toast("config exported",true)}
function importConfig(inp){const file=inp.files&&inp.files[0];if(!file)return;const reader=new FileReader();reader.onload=()=>{try{const json=JSON.parse(reader.result);restoreBackup(json)}catch(e){toast("invalid file: "+e.message,false)}};reader.readAsText(file);inp.value=""}
async function restoreBackup(json){if(!json||typeof json!=="object"){toast("invalid backup",false);return}const r=await fetch("/api/action",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:"restore_backup",data:json})});const res=await r.json();toast(res.msg,res.ok);load()}
async function pollEvents(){const r=await fetch("/api/events");const j=await r.json();if(j.mode){D.mode=j.mode}
 // refresh device list so newly discovered nodes appear without a manual page refresh
 const dr=await fetch("/api/data");const dj=await dr.json();
 const added=D.devices.filter(d=>!dj.devices.some(n=>n.id===d.id));
 const removed=D.devices.some(d=>!dj.devices.some(n=>n.id===d.id));
 const statusChanged=D.devices.length!==dj.devices.length||D.devices.some((d,i)=>dj.devices[i]&&d.status!==dj.devices[i].status);
 if((added.length||removed||statusChanged)&&cur==="devices"){D.devices=dj.devices;if($("main"))devices_render()}
  if($("mon") && MON_RUNNING){const f=($("mon_filter")?$("mon_filter").value:"").trim().toLowerCase();let list=(dj.events||[]).slice(0,120);if(f){list=list.filter(e=>((e.source||"")+(e.dest||"")+(e.proto||"")+(e.msg||"")+(e.ts||"")).toLowerCase().indexOf(f)>=0)}const ev=list.map(e=>"<div class='monrow'><span class='t'>"+esc(e.ts)+"</span><span class='s'>"+esc(e.source)+"</span><span class='ip'>"+esc(e.src_ip||"-")+"</span><span class='s'>"+esc(e.dest||"-")+"</span><span class='ip'>"+esc(e.dst_ip||"-")+"</span><span class='pro'>"+esc(e.proto||"-")+"</span><span>"+esc(e.msg||e.data||"")+"</span></div>").join("");$("mon").innerHTML=ev||""}
 D.events=dj.events;D.devices=dj.devices;
 // refresh sensor values live so the Sensors page updates in real time
 if(JSON.stringify(D.sensors)!==JSON.stringify(dj.sensors)){D.sensors=dj.sensors;if(cur==="sensors"&&$("main"))$("main").innerHTML=sensors()}
 // live refresh of configuration tables without losing in-progress form input
 const dataChanged=JSON.stringify(D.qos_configs)!==JSON.stringify(dj.qos_configs)||JSON.stringify(D.vlan_groups)!==JSON.stringify(dj.vlan_groups)||JSON.stringify(D.tas_schedules)!==JSON.stringify(dj.tas_schedules)||JSON.stringify(D.preemption_configs)!==JSON.stringify(dj.preemption_configs)||JSON.stringify(D.tsn_streams)!==JSON.stringify(dj.tsn_streams)||JSON.stringify(D.gcl_entries)!==JSON.stringify(dj.gcl_entries);
 D.qos_configs=dj.qos_configs;D.vlan_groups=dj.vlan_groups;D.tas_schedules=dj.tas_schedules;D.preemption_configs=dj.preemption_configs;D.tsn_streams=dj.tsn_streams;D.tsn_stream_members=dj.tsn_stream_members;D.gcl_entries=dj.gcl_entries;
 if(dataChanged){if(cur==="qos"&&$("main"))$("main").innerHTML=qos();else if(cur==="vlan"&&$("main"))$("main").innerHTML=vlan();else if(cur==="tas"&&$("main"))$("main").innerHTML=tas();else if(cur==="preemption"&&$("main"))$("main").innerHTML=preemption();else if(cur==="streams"&&$("main"))$("main").innerHTML=streams();}}
setInterval(pollEvents,2000);</script></body></html>
"""


if __name__ == "__main__":
    import sys
    import signal

    def usage():
        print("Usage: python3 webgui.py [--host H] [--port P] [--help]")
        print("  --host H      bind address (default %s, use 0.0.0.0 to expose)" % WEB_HOST)
        print("  --port P      port (default %d)" % PORT)
        print("Env: WTSN_HOST, WTSN_PORT, WTSN_DB, WTSN_BROKER, WTSN_USER,")
        print("     WTSN_PASS, WTSN_WEB_USER, WTSN_WEB_PASS")
        return

    host = WEB_HOST
    port = PORT
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] in ("-h", "--help"):
            usage()
            sys.exit(0)
        elif args[i] == "--host" and i + 1 < len(args):
            host = args[i + 1]; i += 2
        elif args[i] == "--port" and i + 1 < len(args):
            port = int(args[i + 1]); i += 2
        else:
            i += 1

    LISTENER_STOP.clear()
    threading.Thread(target=sim_runner, daemon=True).start()
    threading.Thread(target=mqtt_listener_loop, daemon=True).start()

    class WTSNServer(ThreadingHTTPServer):
        daemon_threads = True
        allow_reuse_address = True

    srv = WTSNServer((host, port), make_handler())
    srv.daemon_threads = True

    def _shutdown(sig, frame):
        LISTENER_STOP.set()
        try:
            srv.shutdown()
        except Exception:
            pass
        try:
            os._exit(0)
        except Exception:
            pass
    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    scheme = "http"  # TLS is not bundled; run behind a reverse proxy for TLS.
    addr = "127.0.0.1" if host in ("127.0.0.1", "localhost") else host
    print("WTSN web GUI: %s://%s:%d  (db=%s)" % (scheme, addr, port, DB_REAL), flush=True)
    if host in ("127.0.0.1", "localhost"):
        try:
            import webbrowser
            threading.Thread(target=webbrowser.open,
                           args=("http://127.0.0.1:%d/" % port,),
                           daemon=True).start()
        except Exception:
            pass
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        LISTENER_STOP.set()
        if REAL_MQTT:
            try: REAL_MQTT.close()
            except Exception: pass
        srv.server_close()
        print("WTSN web GUI stopped", flush=True)
