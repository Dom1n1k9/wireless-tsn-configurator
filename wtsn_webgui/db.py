"""SQLite helpers and the in-memory event trace."""
import os
import re
import socket
import sqlite3
import time
import zlib

from . import state

# Canonical device id shown in the UI for every real ESP32-CAM. The firmware
# may announce itself under a free-form id (esp32-cam, esp32-cam-01, ...);
# the GUI collapses all of them onto this single unnumbered camera so the
# "I have one camera" picture stays clean. The real DB row keeps its own id.
CANONICAL_CAM = "esp32-cam"


def is_cam_id(did):
    return bool(did) and re.search(r"cam", str(did), re.I) is not None


def is_cam_row(d):
    if not d:
        return False
    if int(d.get("kind") or 0) == 5:
        return True
    return is_cam_id(d.get("id"))

SCHEMA = (
    "CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY,name TEXT,ip TEXT,mac TEXT,"
    "kind INTEGER,firmware TEXT,status INTEGER,last_seen INTEGER,domain TEXT DEFAULT 'default',"
    "heartbeat_at INTEGER DEFAULT 0,rssi INTEGER DEFAULT 0,usb TEXT DEFAULT '');"
    "CREATE TABLE IF NOT EXISTS firmware(file TEXT PRIMARY KEY,version TEXT,size INTEGER,"
    "crc32 TEXT,kind INTEGER DEFAULT -1,uploaded_at INTEGER);"
    "CREATE TABLE IF NOT EXISTS fx_data(id INTEGER PRIMARY KEY AUTOINCREMENT,ts INTEGER,"
    "src TEXT,data_id TEXT,value REAL,text TEXT);"
    "CREATE TABLE IF NOT EXISTS domains(id TEXT PRIMARY KEY,name TEXT,description TEXT);"
    "CREATE TABLE IF NOT EXISTS device_tsn_features(device_id TEXT,feature TEXT);"
    "CREATE TABLE IF NOT EXISTS qos_configs(device_id TEXT PRIMARY KEY,priority INTEGER,"
    "traffic_class INTEGER,bandwidth_kbps INTEGER,latency_ms INTEGER,preemption INTEGER);"
    "CREATE TABLE IF NOT EXISTS preemption_configs(device_id TEXT PRIMARY KEY,preemption "
    "INTEGER,emac TEXT,pmac TEXT);"
    "CREATE TABLE IF NOT EXISTS vlan_groups(id TEXT PRIMARY KEY,name TEXT,vlan_id INTEGER);"
    "CREATE TABLE IF NOT EXISTS vlan_members(group_id TEXT,device_id TEXT);"
    "CREATE TABLE IF NOT EXISTS tas_schedules(id TEXT PRIMARY KEY,name TEXT,cycle_time_ns "
    "INTEGER,deploy_target TEXT);"
    "CREATE TABLE IF NOT EXISTS gcl_entries(schedule_id TEXT,\"index\" INTEGER,gate_state "
    "INTEGER,duration_ns INTEGER);"
    "CREATE TABLE IF NOT EXISTS timesync_status(id TEXT PRIMARY KEY,mode INTEGER,"
    "grandmaster TEXT,offset_ns INTEGER,quality INTEGER,jitter_ns INTEGER DEFAULT 0);"
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
    "CREATE TABLE IF NOT EXISTS sensor_history(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "device_id TEXT,sensor_id TEXT,ts INTEGER,value REAL);"
    "CREATE TABLE IF NOT EXISTS latency_log(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "device_id TEXT,ts INTEGER,latency_ms REAL);"
    "CREATE TABLE IF NOT EXISTS recordings(device_id TEXT,path TEXT,recorded_at INTEGER,"
    "PRIMARY KEY(device_id,path));"
    "CREATE TABLE IF NOT EXISTS sonar_sweeps(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "device_id TEXT,ts INTEGER,sweep_id INTEGER,sweep TEXT);"
    "CREATE TABLE IF NOT EXISTS ai_decisions(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "ts INTEGER,device_id TEXT,action TEXT,params TEXT,reason TEXT,source TEXT,"
    "status TEXT);"
)


def fw_version_from_name(name):
    m = re.search(r"v?(\d+\.\d+\.\d+)", name or "")
    return m.group(1) if m else ""


def crc32_hex(data):
    return "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)


def ensure_schema(con):
    con.executescript(SCHEMA)
    con.commit()
    for tbl, col in (("devices", "domain"), ("devices", "heartbeat_at"),
                      ("devices", "rssi"), ("devices", "usb"),
                      ("devices", "last_deploy_at"), ("devices", "last_deploy_ok"),
                      ("timesync_status", "jitter_ns")):
        try:
            cols = [r[1] for r in con.execute("PRAGMA table_info(%s)" % tbl)]
            if col not in cols:
                con.execute("ALTER TABLE %s ADD COLUMN %s" % (tbl, col))
        except Exception:
            pass
    con.commit()
    try:
        # Register firmware images already on disk (uploaded before the
        # firmware table existed) so the GUI can list and flash them.
        have = {r[0] for r in con.execute("SELECT file FROM firmware")}
        for f in os.listdir(state.FW_DIR):
            if f in have or not f.endswith((".bin", ".img", ".hex")):
                continue
            p = os.path.join(state.FW_DIR, f)
            if not os.path.isfile(p):
                continue
            with open(p, "rb") as fh:
                data = fh.read()
            con.execute("INSERT INTO firmware(file,version,size,crc32,kind,uploaded_at)"
                        " VALUES(?,?,?,?,?,?)",
                        (f, fw_version_from_name(f), len(data),
                         crc32_hex(data), -1, int(os.path.getmtime(p))))
        con.commit()
    except Exception:
        pass
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
    # Older DBs created 'timesync_status' and 'qos_configs' without primary keys,
    # so INSERT OR REPLACE appended duplicate rows (the simulator writes a
    # timesync row every tick). Rebuild deduplicated tables if needed; new DBs
    # get the primary keys from SCHEMA directly.
    for table, key, ddl in (
            ("timesync_status", "id",
             "CREATE TABLE timesync_status_tmp(id TEXT PRIMARY KEY,mode INTEGER,"
             "grandmaster TEXT,offset_ns INTEGER,quality INTEGER,jitter_ns "
             "INTEGER DEFAULT 0)"),
            ("qos_configs", "device_id",
             "CREATE TABLE qos_configs_tmp(device_id TEXT PRIMARY KEY,priority "
             "INTEGER,traffic_class INTEGER,bandwidth_kbps INTEGER,latency_ms "
             "INTEGER,preemption INTEGER)")):
        try:
            rows = con.execute("PRAGMA table_info(%s)" % table).fetchall()
            if rows and not any(r[5] for r in rows):
                con.execute("DROP TABLE IF EXISTS %s_tmp" % table)
                con.execute(ddl)
                cols = ",".join('"%s"' % r[1] for r in rows)
                con.execute(
                    "INSERT OR REPLACE INTO %s_tmp(%s) SELECT %s FROM %s AS t WHERE "
                    "t.rowid=(SELECT MAX(x.rowid) FROM %s AS x WHERE x.\"%s\"=t.\"%s\")"
                    % (table, cols, cols, table, table, key, key))
                con.execute("DROP TABLE %s" % table)
                con.execute("ALTER TABLE %s_tmp RENAME TO %s" % (table, table))
                con.commit()
        except Exception:
            pass
    # Indexes for the telemetry/history hot paths (sensor sparklines, perf stats,
    # sync reports, latency samples). Full-table scans are avoidable here.
    for ddl in (
        "CREATE INDEX IF NOT EXISTS ix_sensor_history_dev ON sensor_history(device_id, ts)",
        "CREATE INDEX IF NOT EXISTS ix_latency_log_dev ON latency_log(device_id, ts)",
        "CREATE INDEX IF NOT EXISTS ix_sync_reports_dev ON timesync_reports(device_id, ts)",
    ):
        try:
            con.execute(ddl)
        except Exception:
            pass
    con.commit()


def connect():
    con = sqlite3.connect(state.DB_SIM if state.MODE["mode"] == "sim" else state.DB_REAL,
                          timeout=3)
    con.row_factory = sqlite3.Row
    try:
        con.execute("PRAGMA journal_mode=WAL")
        con.execute("PRAGMA busy_timeout=5000")
        con.execute("PRAGMA synchronous=NORMAL")
        con.execute("PRAGMA foreign_keys=ON")
    except sqlite3.Error:
        pass
    ensure_schema(con)
    return con


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
    with state.EVENT_LOCK:
        state.EVENTS.appendleft({"ts": time.strftime("%H:%M:%S"), "kind": kind,
                                  "source": source, "msg": msg,
                                  "src_ip": src_ip, "dst_ip": dst_ip, "dest": dest,
                                  "proto": proto})
    state.WS_NOTIFY.set()


def load_all():
    out = {}
    con = connect()
    try:
        for t in state.TABLES:
            try:
                out[t] = [dict(r) for r in con.execute("SELECT * FROM %s" % t)]
            except sqlite3.Error:
                out[t] = []
        # Collect all camera nodes on the network. A real ESP32-CAM never sends
        # the periodic MQTT heartbeat a plain agent does, so its DB row would
        # look stale and get flagged offline. Cameras are instead considered
        # online while their own HTTP still answers (checked lazily on ping).
        # Stale-detection below therefore exempts rows that carry an IP and are
        # a camera.
        cams = {d["id"]: d for d in out.get("devices", []) if is_cam_row(d)}
        # stale detection: a device that has not reported within OFFLINE_AFTER
        # seconds is shown as offline even if it is still marked online in the DB.
        now = int(time.time())
        for d in out.get("devices", []):
            ls = d.get("last_seen") or 0
            if d["id"] in cams:
                # A camera shows online as long as it has an IP it can be
                # reached at; its own HTTP answers live to the ping button.
                if d.get("ip"):
                    d["status"] = 0
                d["is_cam"] = True
                continue
            if ls and (now - ls) > state.OFFLINE_AFTER and state.MODE["mode"] == "real":
                d["status"] = 1
        # Normalize camera ids: every real camera is presented in the UI as the
        # single unnumbered 'esp32-cam'. Only the canonical id is kept on the
        # client; the extra numbered rows are dropped so the user never sees
        # "I have 2 cams" (there is only the one physical camera).
        devs, seen_cam = [], set()
        for d in out.get("devices", []):
            if d["id"] in cams and d["id"] != CANONICAL_CAM:
                d["real_id"] = d["id"]   # keep the real row id for addressing
                d["id"] = CANONICAL_CAM
            if d["id"] in seen_cam:
                continue  # already shown under the canonical id
            seen_cam.add(d["id"])
            devs.append(d)
        out["devices"] = devs
        # Normalize recording rows the same way: the cam announces clips under
        # its real id, but the UI addresses the camera as 'esp32-cam'.
        for r in out.get("recordings", []):
            if is_cam_id(r.get("device_id")) and r["device_id"] != CANONICAL_CAM:
                r["device_id"] = CANONICAL_CAM
        out["cameras"] = []
        seen_ip = set()
        for c in cams.values():
            ip = c.get("ip") or ""
            if not ip or ip in seen_ip:
                continue
            seen_ip.add(ip)
            out["cameras"].append({"id": CANONICAL_CAM, "ip": ip, "camera_row": True})
        # Let the UI show when real mode is selected but the broker is unreachable.
        out["broker_ok"] = state.BROKER.get("ok", False) if state.MODE["mode"] == "real" else True
    finally:
        con.close()
    return out


def get_events():
    with state.EVENT_LOCK:
        return {"mode": state.MODE["mode"], "events": list(state.EVENTS)[:300]}


def sensor_history(did, limit=288):
    """Return recent history per sensor: {sensor_id: [[ts, value], ...]} (oldest first)."""
    con = connect()
    try:
        out = {}
        rows = con.execute(
            "SELECT sensor_id,ts,value FROM sensor_history WHERE device_id=? "
            "ORDER BY ts DESC LIMIT ?", (did, limit)).fetchall()
        for r in rows:
            out.setdefault(r["sensor_id"], []).append([r["ts"], r["value"]])
        for k in out:
            out[k].reverse()
        return out
    finally:
        con.close()


def clamp(v, lo, hi):
    try:
        return max(lo, min(hi, int(v)))
    except (TypeError, ValueError):
        return lo
