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
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

# --- minimal MQTT 3.1.1 client (stdlib only, no paho dependency) ---
class MqttBroker:
    """Minimal MQTT 3.1.1 client: connect, subscribe, publish, keepalive.
    Used in real mode to send commands to the ESP32 agent via a broker."""
    def __init__(self, host="127.0.0.1", port=1883, client_id="wtsn-webgui"):
        self.host, self.port, self.client_id = host, port, client_id
        self.username = os.environ.get("WTSN_USER", "")
        self.password = os.environ.get("WTSN_PASS", "")
        self._sock = None
        self._msg_id = 1
        self._lock = threading.Lock()

    def _write(self, hdr, body):
        # variable header: byte 1 is remaining length
        remaining = len(body)
        enc = bytearray([(remaining % 128) | (0x80 if remaining >= 128 else 0)])
        if remaining >= 128:
            enc.append(remaining // 128)
        self._sock.sendall(hdr + bytes(enc) + body)

    def _read_packet(self):
        sock = self._sock
        # fixed header
        fh = sock.recv(2)
        if len(fh) < 2: return None
        rem = fh[1]
        mult, rem_len = 1, rem
        while rem & 0x80:
            r = sock.recv(1)
            if not r: return None
            rem_len += (r[0] & 0x7f) * mult
            mult *= 128
            rem = r[0]
        body = b""
        while len(body) < rem_len:
            chunk = sock.recv(rem_len - len(body))
            if not chunk: return None
            body += chunk
        return fh[0], body

    def _send_str(self, body, s):
        b = s.encode("utf-8")
        return body + struct.pack(">H", len(b)) + b

    def connect(self):
        with self._lock:
            try:
                self._sock = socket.create_connection((self.host, self.port), timeout=5)
            except OSError:
                self._sock = None
                return False
            # CONNECT (MQTT 3.1.1)
            flags = 0x02  # clean session
            vh = b"\x00\x04MQTT\x04" + bytes([flags]) + struct.pack(">H", 30)
            payload = self._send_str(b"", self.client_id)
            if self.username:
                flags |= 0x80
                vh = b"\x00\x04MQTT\x04" + bytes([flags]) + struct.pack(">H", 30)
                payload = self._send_str(payload, self.username)
                payload = self._send_str(payload, self.password)
            body = vh + payload
            self._write(b"\x10", body)
            resp = self._read_packet()
            ok = resp and resp[0] == 0x20 and len(resp[1]) > 0 and resp[1][0] == 0
            if not ok:
                self._sock.close(); self._sock = None
            return ok

    def publish(self, topic, payload, qos=0):
        if not self._sock: return False
        with self._lock:
            body = self._send_str(b"", topic)
            body += bytes([qos << 6])
            body += payload.encode("utf-8") if isinstance(payload, str) else payload
            try:
                self._write(b"\x30" if qos == 0 else b"\x32", body)
                return True
            except OSError:
                self._sock = None
                return False

    def subscribe(self, topic, qos=0):
        if not self._sock: return False
        with self._lock:
            self._msg_id += 1
            body = struct.pack(">H", self._msg_id) + self._send_str(b"", topic) + bytes([qos])
            try:
                self._write(b"\x82", body)
                return True
            except OSError:
                self._sock = None
                return False

    def close(self):
        with self._lock:
            if self._sock:
                try: self._sock.close()
                except OSError: pass
                self._sock = None

    def recv_publish(self):
        """Read one inbound message. Returns (topic, payload) or None."""
        try:
            pkt = self._read_packet()
        except OSError:
            pkt = None
        if not pkt:
            try: self.close()
            except Exception: pass
            return None
        ptype, body = pkt
        if (ptype >> 4) == 3:
            qos = (ptype >> 1) & 0x3
            tl = struct.unpack(">H", body[:2])[0]
            topic = body[2:2 + tl].decode("utf-8", "replace")
            off = 2 + tl
            if qos > 0:
                off += 2
            payload = body[off:].decode("utf-8", "replace")
            return topic, payload
        return None

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
    while not LISTENER_STOP.is_set():
        try:
            con = sqlite3.connect(DB_REAL if MODE["mode"] == "real" else DB_SIM, timeout=3)
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
            if cons is None:
                cons = sqlite3.connect(DB_REAL if MODE["mode"] == "real" else DB_SIM, timeout=3)
                cons.row_factory = sqlite3.Row
                ensure_schema(cons)
            while not LISTENER_STOP.is_set():
                r = brk.recv_publish()
                if r is None:
                    break
                parse_listener_msg(cons, r[0], r[1])
            if cons:
                cons.commit()
        except Exception:
            time.sleep(3)

def parse_listener_msg(con, topic, payload):
    try:
        j = json.loads(payload)
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
          "settings"]

SCHEMA = (
    "CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY,name TEXT,ip TEXT,mac TEXT,"
    "kind INTEGER,firmware TEXT,status INTEGER,last_seen INTEGER);"
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
    "offset_ns INTEGER,quality INTEGER);"
    "CREATE TABLE IF NOT EXISTS sensors(device_id TEXT,sensor_id TEXT,type INTEGER,name TEXT,"
    "value REAL,unit TEXT,healthy INTEGER,last_update INTEGER);"
    "CREATE TABLE IF NOT EXISTS tsn_streams(stream_id TEXT PRIMARY KEY,name TEXT,talker TEXT,"
    "vlan_id INTEGER,max_latency_ns INTEGER,max_interval_ns INTEGER,priority INTEGER,"
    "data_frame_prio INTEGER,status INTEGER CHECK(status IN (0,1,2,3)),comment TEXT);"
    "CREATE TABLE IF NOT EXISTS tsn_stream_members(stream_id TEXT,role TEXT,device_id TEXT);"
    "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT);"
)

def ensure_schema(con):
    con.executescript(SCHEMA)
    con.commit()

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
    finally:
        con.close()
    return out


def get_events():
    with EVENT_LOCK:
        return {"mode": MODE["mode"], "events": list(EVENTS)[:300]}


def sim_tick():
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
        for t in ["devices", "qos_configs", "vlan_groups", "vlan_members",
                  "tas_schedules", "gcl_entries", "timesync_status", "sensors",
                  "device_tsn_features", "preemption_configs", "tsn_streams",
                  "tsn_stream_members"]:
            con.execute("DELETE FROM %s" % t)
        for k, kv in kept.items():
            con.execute("INSERT OR REPLACE INTO devices(id,name,ip,mac,kind,firmware,status,"
                        "last_seen) VALUES(?,?,?,?,?,?,?,?)",
                        (k, kv.get("name", ""), kv.get("ip", ""), kv.get("mac", ""),
                         kv.get("kind", 0), kv.get("firmware", ""), kv.get("status", 0),
                         kv.get("last_seen", int(time.time()))))
        n = random.randint(3, 6)
        devs = []
        for i in range(1, n + 1):
            kind, base, ip, name = random.choice(PROFILES)
            did = "%s-%02d" % (base, i)
            con.execute("INSERT INTO devices(id,name,ip,mac,kind,firmware,status,last_seen)"
                        " VALUES(?,?,?,?,?,?,0,strftime('%s','now'))",
                        (did, name, ip, "AA:BB:CC:%02d:%02d" % (i, kind), kind,
                         "%d.%d.%d" % (random.randint(1, 5), random.randint(0, 9),
                                           random.randint(0, 9))))
            for f in random.sample(TSN_FUNCS, random.randint(4, len(TSN_FUNCS))):
                con.execute("INSERT INTO device_tsn_features(device_id,feature) VALUES(?,?)",
                            (did, f))
            cid = "grp%d" % (i % 3 + 1)
            con.execute("INSERT OR IGNORE INTO vlan_groups(id,name,vlan_id) VALUES(?,?,?)",
                        (cid, ["Control", "Media", "Bulk"][i % 3], 100 + i * 100))
            con.execute("INSERT OR IGNORE INTO vlan_members(group_id,device_id) VALUES(?,?)",
                        (cid, did))
            con.execute("INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
                        "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?)",
                        (did, random.randint(0, 7), random.randint(0, 3),
                         random.randint(1000, 50000), random.randint(1, 8),
                         random.randint(0, 1)))
            con.execute("INSERT OR REPLACE INTO preemption_configs(device_id,preemption,emac,pmac)"
                        " VALUES(?,?,?,?)",
                        (did, random.randint(0, 1), "7,6,5", "3,2,1,0"))
            for sid, typ, unit in (("temp1", 0, "C"), ("press1", 1, "hPa"),
                                   ("imu1", 2, "g"), ("gpio1", 4, "V")):
                if random.random() < 0.7:
                    val = (round(random.uniform(15, 35), 1) if typ == 0 else
                           round(random.uniform(990, 1020), 1) if typ == 1 else
                           round(random.uniform(0, 0.5), 2))
                    con.execute("INSERT OR REPLACE INTO sensors(device_id,sensor_id,type,name,"
                                "value,unit,healthy,last_update) VALUES(?,?,?,?,?,?,1,strftime('%s','now'))",
                                (did, sid, typ, sid, val, unit))
            devs.append(did)
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
        con.execute("INSERT OR REPLACE INTO tas_schedules(id,name,cycle_time_ns,deploy_target)"
                    " VALUES(?,?,?,?)", ("sched1", "Control cycle", 1000000, gm))
        con.execute("DELETE FROM gcl_entries WHERE schedule_id='sched1'")
        for idx, (g, d) in enumerate([(0x01, 300000), (0x03, 200000), (0x00, 500000)]):
            con.execute("INSERT INTO gcl_entries(schedule_id,\"index\",gate_state,duration_ns)"
                        " VALUES('sched1',?,?,?)", (idx, g, d))
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
            MODE["mode"] = "sim" if body.get("mode") == "sim" else "real"
            EVENTS.clear()
            add_event("config", "cnc",
                     "mode = SIMULATION" if MODE["mode"] == "sim" else "mode = REAL (waiting for real devices)")
            return {"ok": True, "msg": "mode = " + MODE["mode"]}
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
                            "last_seen) VALUES(?,?,?,?,?,?,?,strftime('%s','now'))",
                            (dev["id"], dev.get("name", ""), dev.get("ip", ""),
                             dev.get("mac", ""), clamp(dev.get("kind", 0), 0, 3),
                             dev.get("firmware", ""), clamp(dev.get("status", 0), 0, 2)))
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
        if act == "save_vlan":
            vlan = clamp(body.get("vlan_id", 1), 1, 4094)
            gid = body.get("id") or ("grp%d" % vlan)
            con.execute("INSERT OR REPLACE INTO vlan_groups(id,name,vlan_id) VALUES(?,?,?)",
                        (gid, body.get("name", ""), vlan))
            con.commit()
            add_event("vlan", "cnc", "802.1Q vlan_id %d %s deployed" % (vlan, gid),
                     src_ip="", dst_ip="", dest="", proto="IEEE 802.1Q (WVLAN)")
            return {"ok": True, "msg": "VLAN group saved"}
        if act == "delete_vlan":
            con.execute("DELETE FROM vlan_groups WHERE id=?", (body.get("id"),))
            con.execute("DELETE FROM vlan_members WHERE group_id=?", (body.get("id"),))
            con.commit()
            return {"ok": True, "msg": "VLAN deleted"}
        if act == "save_member":
            con.execute("INSERT OR REPLACE INTO vlan_members(group_id,device_id) VALUES(?,?)",
                        (body.get("group_id"), body.get("device_id")))
            con.commit()
            return {"ok": True, "msg": "Member added"}
        if act == "save_tas":
            cid = body.get("id") or ("sched%d" % int(time.time()))
            con.execute("INSERT OR REPLACE INTO tas_schedules(id,name,cycle_time_ns,deploy_target)"
                        " VALUES(?,?,?,?)",
                        (cid, body.get("name", ""), clamp(body.get("cycle_time_ns", 1000000), 0, 10**12),
                         body.get("deploy_target", "")))
            con.execute("DELETE FROM gcl_entries WHERE schedule_id=?", (cid,))
            for i, e in enumerate(body.get("gcl") or []):
                con.execute("INSERT INTO gcl_entries(schedule_id,index,gate_state,duration_ns)"
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
            add_event("config", "cnc", "802.1Qcc stream %s deployed" % sid)
            return {"ok": True, "msg": "Stream marked ready"}
        if act == "deploy_all_streams":
            con.execute("UPDATE tsn_streams SET status=1")
            con.commit()
            n = con.execute("SELECT COUNT(*) FROM tsn_streams").fetchone()[0]
            add_event("config", "cnc", "802.1Qcc all %d streams deployed" % n)
            return {"ok": True, "msg": "%d streams deployed" % n}
        if act == "fx_send":
            b = get_real_mqtt(con) if MODE["mode"] == "real" else None
            add_event("fx", body.get("source", "cnc"),
                      ("tsn/fx/field <- " + body.get("msg", "")))
            if b:
                b.publish("tsn/fx/field", body.get("msg", ""))
                return {"ok": True, "msg": "FX published on broker"}
            return {"ok": True, "msg": "FX sent (no broker / simulation)"}
        if act == "clear_events":
            EVENTS.clear()
            return {"ok": True, "msg": "monitor cleared"}
        return {"ok": False, "msg": "unknown action"}
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
#execbar{display:none;position:fixed;left:0;right:0;bottom:0;z-index:20;background:var(--surf);border-top:1px solid var(--sec);padding:10px 16px}
#execbar h3{margin:0 0 8px;color:var(--sec);font-size:13px}
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
</style></head><body>
<header><span class="logo">WTSN Configurator</span><span class="sub">Wireless TSN control plane</span>
<div class="spacer"></div>
<span class="mode"><button id="md_real" onclick="setMode('real')">Real</button><button id="md_sim" class="on" onclick="setMode('sim')">Simulation</button></span>
</header>
<div class="wrap"><nav id="nav"></nav><main id="main"></main></div>
<div id="execbar"><h3>Controller / FXMQTT target</h3><button class="big" onclick="execAll()">Execute settings on controller</button></div>
<div id="toast"></div>
<script>
const NAV=[["System",[["devices","Devices"],["monitor","Monitor"],["sensors","Sensors"]]],["OPC UA FX over MQTT",[["fxmqtt","FXMQTT Config"]]],["IEEE 802.1AS",[["timesync","Synchronization"]]],["IEEE 802.1Q",[["qos","QoS Priority"],["vlan","VLAN ID"]]],["IEEE 802.1Qbv",[["tas","TAS / GCL"]]],["IEEE 802.1Qbu",[["preemption","Preemption"]]],["IEEE 802.1Qcc",[["streams","TSN Streams"]]]];
let D={},cur="devices";
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
function types(d){return (D.device_tsn_features||[]).filter(f=>f.device_id==d).map(f=>f.feature).join(", ")}
function tsnOpts(gm){return D.devices.map(d=>"<label><input type=checkbox value='"+esc(d.id)+"' "+(d.id==gm?"checked":"")+"> GM:"+esc(d.id)+"</label>").join("")}
function go(p){cur=p;document.querySelectorAll("#nav button").forEach(b=>b.className=b.id==="nv_"+p?"on":"");
 const m=$("main");
 if(p==="devices")m.innerHTML=devices();
 else if(p==="qos")m.innerHTML=qos();
 else if(p==="vlan")m.innerHTML=vlan();
 else if(p==="tas")m.innerHTML=tas();
 else if(p==="preemption")m.innerHTML=preemption(); else if(p==="timesync"){m.innerHTML=timesync();redrawSetup();}
 else if(p==="streams")m.innerHTML=streams();
 else if(p==="fxmqtt")m.innerHTML=fxmqtt();
 else if(p==="monitor"){m.innerHTML=monitor();refreshMon();}
 else if(p==="sensors")m.innerHTML=sensors();
 else if(p==="settings")m.innerHTML=settings();
 showExec()}
function showExec(){const c=document.getElementById("execbar");if(c)c.style.display=$("main").innerHTML?"block":"none"}
function execAll(){api("exec_all",{}).then(load)}
function devices(){
 const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 const rows=D.devices.map(d=>"<tr><td>"+esc(d.id)+"</td><td>"+esc(d.name)+"</td><td>"+(["online","offline","error"][d.status]||d.status)+"</td><td>"+esc(d.ip)+"</td><td>"+esc((D.device_tsn_features||[]).filter(f=>f.device_id==d.id).map(f=>f.feature).join(", "))+"</td><td><button class='danger' onclick='api(\"save_devices\",{delete:[\""+esc(d.id)+"\"]}).then(load)'>Del</button></td></tr>").join("");
 return "<h2>Devices</h2><div class='card'><h3>Add TSN device</h3>"+
  "<div class='row'><label>id</label><input id=did></div>"+
  "<div class='row'><label>name</label><input id=dname></div>"+
  "<div class='row'><label>ip</label><input id=dip></div>"+
  "<button onclick=saveDev()>Add Device</button></div>"+
  "<h3>Devices</h3><table><tr><th>id</th><th>name</th><th>status</th><th>ip</th><th>tsn</th><th></th></tr>"+rows+"</table>"}
function devices_render(){const hdr=[].slice.call(document.querySelectorAll("h3")).find(h=>h.textContent==="Devices");const tbl=hdr?hdr.nextElementSibling:null;if(tbl&&tbl.tagName==="TABLE"){const rows=D.devices.map(d=>"<tr><td>"+esc(d.id)+"</td><td>"+esc(d.name)+"</td><td>"+(["online","offline","error"][d.status]||d.status)+"</td><td>"+esc(d.ip)+"</td><td>"+esc((D.device_tsn_features||[]).filter(f=>f.device_id==d.id).map(f=>f.feature).join(", "))+"</td><td><button class='danger' onclick='api(\"save_devices\",{delete:[\""+esc(d.id)+"\"]}).then(load)'>Del</button></td></tr>").join("");tbl.innerHTML="<tr><th>id</th><th>name</th><th>status</th><th>ip</th><th>tsn</th><th></th></tr>"+rows}}
function saveDev(){api("save_devices",{device:{id:$("did").value,name:$("dname").value,ip:$("dip").value,firmware:"",kind:0,status:0,tsn:[]}})}
const PRIOS=[[0,"Background (background data)"],[1,"Best effort"],[2,"Excellent effort"],[3,"Critical application"],[4,"Video (latency and jitter below 100 ms)"],[5,"Voice (latency and jitter below 10 ms)"],[6,"Internetwork control (network control)"],[7,"Control data traffic (data traffic control)"]];
function qos(){const def=(D.qos_configs[0]||{}).priority;
 let rows=D.qos_configs.map(q=>"<tr><td>"+esc(q.device_id)+"</td><td>Priority "+q.priority+" — "+(PRIOS.find(p=>p[0]==q.priority)||["",q.priority])[1]+"</td><td>"+q.bandwidth_kbps+"</td><td>"+q.latency_ms+" ms</td></tr>").join("");
 const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 const prio=PRIOS.map(p=>"<option value='"+p[0]+"' "+(String(def)==String(p[0])?"selected":"")+">Priority "+p[0]+" — "+esc(p[1])+"</option>").join("");
 return "<h2>IEEE 802.1Q — QoS Priority</h2><div class='card'>"+
  "<div class='row'><label>device</label><select id=q_dev><option value=''>-</option>"+opts+"</select></div>"+
  "<div class='row'><label>priority</label><select id=q_prio>"+prio+"</select></div>"+
  "<div class='row'><label>bandwidth kbps</label><input id=q_bw type=number value=1000></div>"+
  "<div class='row'><label>latency ms</label><input id=q_lat type=number value=2></div>"+
  "<button onclick=saveQ()>Save QoS</button></div>"+
  "<h3>Current settings</h3><div class=card><p class=muted>Traffic class follows the priority automatically. Preemption is configured on the <b>IEEE 802.1Qbu</b> page.</p></div>"+
  "<table><tr><th>device</th><th>priority</th><th>bw kbps</th><th>latency ms</th></tr>"+rows+"</table>"}
function saveQ(){const d=$("q_dev").value;const p=parseInt($("q_prio").value);api("save_qos",{device_id:d,priority:p,traffic_class:p,bandwidth_kbps:parseInt($("q_bw").value),latency_ms:parseInt($("q_lat").value),preemption:(D.qos_configs.find(q=>q.device_id==d)||{}).preemption||0})}
function vlan(){let rows=D.vlan_groups.map(g=>"<tr><td>"+esc(g.id)+"</td><td>"+esc(g.name)+"</td><td>"+g.vlan_id+"</td><td>"+esc((D.vlan_members||[]).filter(x=>x.group_id==g.id).map(x=>x.device_id).join(", "))+"</td></tr>").join("");
 return "<h2>VLAN (IEEE 802.1Q)</h2><div class='card'><div class='row'><label>name</label><input id=v_name></div><div class='row'><label>VLAN ID 1-4094</label><input id=v_id type=number value=100></div><button onclick='api(\"save_vlan\",{id:\"v_\"+Math.random().toString(36).slice(2,6),name:$(\"v_name\").value,vlan_id:parseInt($(\"v_id\").value)})'>Add group</button></div><table><tr><th>id</th><th>name</th><th>vlan</th><th>members</th></tr>"+rows+"</table>"}
function tas(){let bl=D.tas_schedules.map(s=>{const g=(D.gcl_entries||[]).filter(e=>e.schedule_id==s.id).map(e=>e.gate_state+":"+e.duration_ns).join(", ");return "<div class='card'><h3>"+esc(s.name)+"</h3><div class='row'><label>id</label><span>"+esc(s.id)+"</span></div><div class='row'><label>cycle</label><span>"+s.cycle_time_ns+" ns</span></div><div class='row'><label>deploy</label><span>"+esc(s.deploy_target)+"</span></div><div class='row'><label>GCL</label><span>"+esc(g)+"</span></div></div>"}).join("");
 const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 return "<h2>TAS (IEEE 802.1Qbv) — Gate Control List</h2><div class='card'><div class='row'><label>name</label><input id=t_name></div><div class='row'><label>cycle ns</label><input id=t_cyc type=number value=1000000></div><div class='row'><label>deploy target</label><select id=t_dev>"+opts+"</select></div><div class='row'><label>GCL</label><input id=t_gcl value='1:300000,3:200000,0:500000'></div><button onclick=tSave()>Save / Deploy</button></div>"+bl}
function tSave(){const g=$("t_gcl").value.split(",").map(x=>{const[a,b]=x.split(":");return{gate_state:parseInt(a),duration_ns:parseInt(b)}});api("save_tas",{id:"s",name:$("t_name").value||"schedule",cycle_time_ns:parseInt($("t_cyc").value),deploy_target:$("t_dev").value,gcl:g})}
function preemption(){
 const rows=(D.preemption_configs||[]).map(p=>"<tr><td>"+esc(p.device_id)+"</td><td>"+(["off","on","on"][p.preemption]||p.preemption)+"</td><td>"+esc(p.emac)+"</td><td>"+esc(p.pmac)+"</td></tr>").join("");
 const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 return "<h2>IEEE 802.1Qbu — Frame Preemption</h2>"+
  "<div class='card'><div class='row'><label>device</label><select id=pr_dev><option value=''>-</option>"+opts+"</select></div>"+
  "<div class='row'><label>mode</label><select id=pr_mode onchange=prMode()><option value=0>off (no preemption)</option><option value=1>on</option></select></div>"+
  "<div id=pr_mac style='display:none'>"+
  "<div class='row'><label>eMAC priorities</label><input id=pr_emac placeholder='e.g. 7,6,5'></div>"+
  "<div class='row'><label>pMAC priorities</label><input id=pr_pmac placeholder='e.g. 3,2,1,0'></div></div>"+
  "<button onclick=savePre()>Save Preemption</button></div>"+
  "<h3>Current settings</h3><table><tr><th>device</th><th>mode</th><th>eMAC prios</th><th>pMAC prios</th></tr>"+rows+"</table>"}
function prMode(){const on=$("pr_mode").value=="1";$("pr_mac").style.display=on?"block":"none"}
function savePre(){const d=$("pr_dev").value;if(!d)return;api("save_preemption",{device_id:d,preemption:parseInt($("pr_mode").value),emac:$("pr_emac").value,pmac:$("pr_pmac").value})}
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
function fsSave(){const type=$("fs_type").value;api("set_server",{type:type,id:(type==="node"?$("fs_node").value:""),broker:$("fs_broker").value||"127.0.0.1:1883"})}
function monitor(){return "<h2>Network / Frame Monitor</h2><div class='card'><div class='row'><span>"+(D.mode==="sim"?"Live simulated flow (MQTT, FX over MQTT, raw frames)":"Real traffic — waiting for real nodes")+"</span>"+"<span class='spacer'></span><button class='ghost' onclick='api(\"clear_events\",{})'>Clear</button></div><div class='monrow'><span class='t'>time</span><span class='s'>source</span><span class='ip'>src IP</span><span class='s'>destination</span><span class='ip'>dst IP</span><span class='pro'>protocol</span><span>message</span></div><div id=mon></div></div>"}
function refreshMon(){if($("mon")){const ev=(D.events||[]).slice(0,120).map(e=>"<div class='monrow'><span class='t'>"+esc(e.ts)+"</span><span class='s'>"+esc(e.source)+"</span><span class='ip'>"+esc(e.src_ip||"-")+"</span><span class='s'>"+esc(e.dest||"-")+"</span><span class='ip'>"+esc(e.dst_ip||"-")+"</span><span class='pro'>"+esc(e.proto||"-")+"</span><span>"+esc(e.msg||e.data||"")+"</span></div>").join("");$("mon").innerHTML=ev||"<div class='monrow'><span>no traffic yet</span></div>"}}
function sensors(){let r=D.sensors.map(s=>"<tr><td>"+esc(s.device_id)+"</td><td>"+esc(s.sensor_id)+"</td><td>"+s.value+" "+esc(s.unit)+"</td><td class='"+(s.healthy?"":"') style='color:var(--err)")+"'>"+(s.healthy?"healthy":"fault")+"</td></tr>").join("");
 return "<h2>Sensors ("+(D.mode==="real"?"real":"simulated")+")</h2>"+(D.sensors.length?("<table><tr><th>device</th><th>id</th><th>value</th><th>health</th></tr>"+r+"</table>"):"<div class='card'>"+(D.mode==="sim"?"Waiting for simulation sensors...":"Connect real sensors — none identified yet.")+"</div>")}
function streams(){
  const rows=(D.tsn_streams||[]).map(s=>{const memb=(D.tsn_stream_members||[]).filter(x=>x.stream_id==s.stream_id);
    const talker=(memb.find(x=>x.role==="talker")||{}).device_id||s.talker||"";
    const lsn=memb.filter(x=>x.role==="listener").map(x=>x.device_id).join(", ");
    return "<tr><td>"+esc(s.stream_id)+"</td><td>"+esc(s.name)+"</td><td>"+esc(talker)+"</td><td>"+esc(lsn)+"</td><td>"+(s.vlan_id||0)+"</td><td>"+(["configured","ready","failed","standby"][s.status]||s.status)+"</td>"+
    "<td><button class='ghost' onclick='api(\"deploy_stream\",{stream_id:\""+esc(s.stream_id)+"\"}).then(load)'>Deploy</button>"+
    "<button class='danger' onclick='api(\"delete_stream\",{stream_id:\""+esc(s.stream_id)+"\"}).then(load)'>Del</button></td></tr>"
  }).join("");
  const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
  const lopt=D.devices.map(d=>"<label class=ck><input type=checkbox value='"+esc(d.id)+"'> "+esc(d.id)+"</label>").join("");
  return "<h2>IEEE 802.1Qcc — TSN Streams</h2>"+
   "<div class='card'><h3>Talker / Listener stream reservation</h3>"+
   "<div class='row'><label>name</label><input id=s_name></div>"+
   "<div class='row'><label>talker</label><select id=s_talker><option value=''>-</option>"+opts+"</select></div>"+
   "<div class='row'><label>listeners</label><div id=s_lsn>"+lopt+"</div></div>"+
   "<button onclick=saveStream()>Save Stream</button>"+
   "<button class='ghost' onclick='api(\"deploy_all_streams\",{}).then(load)'>Deploy All</button></div>"+
   "<h3>Streams</h3><table><tr><th>id</th><th>name</th><th>talker</th><th>listeners</th><th>vlan</th><th>status</th><th></th></tr>"+rows+"</table>"}
function saveStream(){const lsn=[].slice.call(document.querySelectorAll("#s_lsn input:checked")).map(c=>c.value);
 api("save_stream",{name:$("s_name").value,talker:$("s_talker").value,listeners:lsn,
  vlan_id:parseInt($("s_vlan")?$("s_vlan").value:0)||0,max_latency_ns:parseInt($("s_lat")?$("s_lat").value:1000000)||1000000,
  max_interval_ns:parseInt($("s_itv")?$("s_itv").value:100000)||100000,
  priority:parseInt($("s_prio")?$("s_prio").value:5)||5,data_frame_prio:parseInt($("s_prio")?$("s_prio").value:5)||5}).then(load)}
function settings(){
 const brk=(D.settings||[]).find(s=>s.key==="broker")||{};
 return "<h2>Settings</h2><div class='card'><div class='row'><label>mode</label><span>"+D.mode+"</span></div>"+
  "<div class='row'><label>MQTT broker</label><input id=broker_in value='"+esc(brk.value||"127.0.0.1:1883")+"'></div>"+
  "<button onclick=saveBroker()>Apply broker</button></div>"+
  "<div class='card'><div class='row'><label>db</label><span>"+esc(D.db||"wtsn_gui.db")+"</span></div>"+
  "<div class='row'><label>push</label><span>in <b>Real</b> mode /apply is sent over MQTT to devices</span></div></div>"}
function saveBroker(){api("set_server",{type:((D.settings||[]).find(s=>s.key==="server_type")||{}).value||"node",id:"",broker:$("broker_in").value}).then(load)}
async function pollEvents(){const r=await fetch("/api/events");const j=await r.json();if(j.mode){D.mode=j.mode}
 // refresh device list so newly discovered nodes appear without a manual page refresh
 const dr=await fetch("/api/data");const dj=await dr.json();
 const added=D.devices.filter(d=>!dj.devices.some(n=>n.id===d.id));
 const removed=D.devices.some(d=>!dj.devices.some(n=>n.id===d.id));
 const statusChanged=D.devices.length!==dj.devices.length||D.devices.some((d,i)=>dj.devices[i]&&d.status!==dj.devices[i].status);
 if((added.length||removed||statusChanged)&&cur==="devices"){D.devices=dj.devices;if($("main"))devices_render()}
 if($("mon")){const ev=(dj.events||[]).slice(0,120).map(e=>"<div class='monrow'><span class='t'>"+esc(e.ts)+"</span><span class='s'>"+esc(e.source)+"</span><span class='ip'>"+esc(e.src_ip||"-")+"</span><span class='s'>"+esc(e.dest||"-")+"</span><span class='ip'>"+esc(e.dst_ip||"-")+"</span><span class='pro'>"+esc(e.proto||"-")+"</span><span>"+esc(e.msg||e.data||"")+"</span></div>").join("");$("mon").innerHTML=ev||""}
 D.events=dj.events;D.devices=dj.devices}
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

    srv = WTSNServer((host, port), make_handler())
    srv.daemon_threads = True

    def _shutdown(sig, frame):
        srv.shutdown()
    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    scheme = "http"  # TLS is not bundled; run behind a reverse proxy for TLS.
    addr = "127.0.0.1" if host in ("127.0.0.1", "localhost") else host
    print("WTSN web GUI: %s://%s:%d  (db=%s)" % (scheme, addr, port, DB_REAL), flush=True)
    if host in ("127.0.0.1", "localhost"):
        try:
            import webbrowser
            webbrowser.open("http://127.0.0.1:%d/" % port)
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
    srv.serve_forever()
