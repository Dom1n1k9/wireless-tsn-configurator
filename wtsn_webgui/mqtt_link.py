"""Real-mode MQTT link: broker access plus the background status listener."""
import json
import os
import sqlite3
import time

from . import state
from .db import add_event, ensure_schema, get_self_ip
from .mqtt_broker import MqttBroker

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
            REAL_MQTT.close()
            REAL_MQTT = None
    if REAL_MQTT is None:
        REAL_MQTT = MqttBroker(host, port, "wtsn-webgui")
        if not REAL_MQTT.connect():
            return None
    return REAL_MQTT


def parse_listener_msg(con, topic, payload):
    try:
        j = json.loads(payload.replace("\r", "").replace("\n", "").strip())
    except Exception:
        j = {}
    did = j.get("id", "")
    fw = j.get("firmware") or j.get("fw")
    ip = j.get("ip")
    kind = j.get("kind")
    try:
        if "/status" in topic and did:
            con.execute("UPDATE devices SET status=0,last_seen=strftime('%s','now'),"
                        "firmware=COALESCE(?,firmware),ip=COALESCE(?,ip) WHERE id=?",
                        (fw, ip, did))
        elif "/lwt" in topic and did:
            con.execute("UPDATE devices SET status=1,last_seen=strftime('%s','now') WHERE id=?",
                        (did,))
            add_event("mqtt", did, "device reported OFFLINE (MQTT last will)")
        elif "/discover" in topic and did:
            # upsert without clobbering user-set columns (domain, heartbeat, ...)
            kind_v = 5 if kind == "cam" else (0 if kind == "esp32" else None)
            con.execute("UPDATE devices SET status=0,last_seen=strftime('%s','now'),"
                        "firmware=COALESCE(?,firmware),ip=COALESCE(?,ip),"
                        "kind=COALESCE(?,kind) WHERE id=?", (fw, ip, kind_v, did))
            if con.rowcount == 0:
                con.execute("INSERT INTO devices(id,name,status,last_seen,firmware,ip,kind) "
                            "VALUES(?,?,0,strftime('%s','now'),?,?,?)",
                            (did, did, fw, ip, kind_v))
        elif "/ack" in topic:
            ok = j.get("ok", False)
            with state.ACK_LOCK:
                state.RECENT_ACKS[did] = (ok, time.time())
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
                ts = int(s.get("ts") or j.get("ts") or time.time())
                con.execute("INSERT INTO sensor_history(device_id,sensor_id,ts,value) "
                            "VALUES(?,?,?,?)", (did, sid, ts, val))
            # keep ~1h of history per device
            con.execute("DELETE FROM sensor_history WHERE device_id=? AND ts<?",
                        (did, int(time.time()) - 3600))
            con.commit()
            return
        con.commit()
        add_event(topic.split("/")[0], did or "broker", topic + " <- " + payload)
    except Exception:
        pass


def mqtt_listener_loop():
    """Background thread: subscribes to status/ack/discover and updates state."""
    brk = host = port = None
    cons = None
    last_db = None
    while not state.LISTENER_STOP.is_set():
        try:
            wanted_db = state.DB_REAL if state.MODE["mode"] == "real" else state.DB_SIM
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
                h, p = addr.rsplit(":", 1)
                p = int(p)
            except Exception:
                h, p = "127.0.0.1", 1883
            if brk is None or h != host or p != port:
                if brk:
                    brk.close()
                brk = MqttBroker(h, p, "wtsn-webgui-listener")
                host, port = h, p
            if not brk.connect():
                time.sleep(3)
                continue
            brk.subscribe("tsn/ack/#")
            brk.subscribe("tsn/status")
            brk.subscribe("tsn/discover")
            brk.subscribe("tsn/lwt/#")
            brk.subscribe("tsn/fx/#")
            brk.subscribe("tsn/ptp")
            brk.subscribe("tsn/fx/stream")
            brk.subscribe("tsn/sensors")
            if cons is None:
                cons = sqlite3.connect(wanted_db, timeout=3)
                cons.row_factory = sqlite3.Row
                ensure_schema(cons)
            prev_db = wanted_db
            while not state.LISTENER_STOP.is_set():
                r = brk.recv_publish(timeout=1.0)
                if r is None:
                    # reconnect to re-evaluate mode / broker address if it changed
                    break
                parse_listener_msg(cons, r[0], r[1])
                # re-evaluate mode / broker after each message; switch DB if changed
                cur_db = state.DB_REAL if state.MODE["mode"] == "real" else state.DB_SIM
                if cur_db != prev_db:
                    prev_db = cur_db
                    break
            if cons:
                cons.commit()
        except Exception:
            time.sleep(3)
