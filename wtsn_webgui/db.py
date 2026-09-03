"""SQLite helpers and the in-memory event trace."""
import socket
import sqlite3
import time

from . import state

SCHEMA = (
    "CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY,name TEXT,ip TEXT,mac TEXT,"
    "kind INTEGER,firmware TEXT,status INTEGER,last_seen INTEGER,domain TEXT DEFAULT 'default',"
    "heartbeat_at INTEGER DEFAULT 0,rssi INTEGER DEFAULT 0);"
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
    "CREATE TABLE IF NOT EXISTS sensor_history(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "device_id TEXT,sensor_id TEXT,ts INTEGER,value REAL);"
)


def ensure_schema(con):
    con.executescript(SCHEMA)
    con.commit()
    for tbl, col in (("devices", "domain"), ("devices", "heartbeat_at"),
                     ("devices", "rssi"), ("timesync_status", "jitter_ns")):
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
        # stale detection: a device that has not reported within OFFLINE_AFTER
        # seconds is shown as offline even if it is still marked online in the DB.
        now = int(time.time())
        for d in out.get("devices", []):
            ls = d.get("last_seen") or 0
            if ls and (now - ls) > state.OFFLINE_AFTER and state.MODE["mode"] == "real":
                d["status"] = 1
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
