"""Misc/system actions: mode, exec_all, monitor, versions, backup/restore."""
import json
import re as _re
import time

from .. import state
from ..db import add_event, sensor_history
from .. import mqtt_link


def _set_mode(con, body):
    m = body.get("mode")
    if m not in ("sim", "real"):
        return {"ok": False, "msg": "mode must be 'sim' or 'real'"}
    state.MODE["mode"] = m
    with state.EVENT_LOCK:
        state.EVENTS.clear()
    add_event("config", "cnc",
              "mode = SIMULATION" if m == "sim" else "mode = REAL (waiting for real devices)")
    return {"ok": True, "msg": "mode = " + m}


def _exec_all(con, body):
    from .core import deploy_stream_msg, stream_payload
    if state.MODE["mode"] != "real":
        return {"ok": False, "msg": "Execute in SIM mode is not possible (no broker). "
                "Switch to REAL mode."}
    broker = mqtt_link.get_real_mqtt(con)
    if not broker:
        return {"ok": False, "msg": "MQTT broker not reachable"}
    snapshots = {}
    n_pub = 0
    for r in con.execute("SELECT * FROM devices"):
        did = r["id"]
        snap = {"cmd": "apply", "id": did, "priority": 0, "traffic_class": 0,
                "vlan_id": 0, "group": "default", "preemption": 0,
                "timesync_mode": 0, "grandmaster": "", "tas_cycle_ns": 0,
                "gcl": []}
        q = con.execute("SELECT * FROM qos_configs WHERE device_id=?", (did,)).fetchone()
        if q:
            snap["priority"] = q["priority"]
            snap["traffic_class"] = q["traffic_class"]
            snap["preemption"] = q["preemption"]
        grp = con.execute("SELECT vlan_id,name FROM vlan_groups WHERE id="
                          "(SELECT group_id FROM vlan_members WHERE device_id=? LIMIT 1)",
                          (did,)).fetchone()
        if grp:
            snap["vlan_id"] = grp["vlan_id"]
            snap["group"] = grp["name"]
        ts = con.execute("SELECT mode,grandmaster FROM timesync_status WHERE id='main'").fetchone()
        if ts:
            gm = ts["grandmaster"] or ""
            if gm and gm != "PC":
                snap["timesync_mode"] = 1 if did == gm else 2
                snap["grandmaster"] = gm
            else:
                snap["timesync_mode"] = ts["mode"]
                snap["grandmaster"] = gm
        tas = con.execute("SELECT * FROM tas_schedules WHERE 1 LIMIT 1").fetchone()
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
    stream_rows = con.execute("SELECT * FROM tsn_streams").fetchall()
    for sr in stream_rows:
        payload = stream_payload(con, sr["stream_id"])
        if not payload:
            continue
        memb = con.execute(
            "SELECT role,device_id FROM tsn_stream_members WHERE stream_id=?",
            (sr["stream_id"],)).fetchall()
        talker = next((m["device_id"] for m in memb if m["role"] == "talker"), "")
        n_pub += deploy_stream_msg(broker, con, sr["stream_id"], payload)
        if talker:
            add_event("fxmqtt", "cnc",
                      "stream %s published -> %s" % (sr["stream_id"], talker))
    retried = []
    pending = []
    # Only acks received *after* we started sending count, otherwise a stale ack
    # from a previous /apply round makes every device look acknowledged.
    start_ack = time.time()
    end = time.time() + 2.0
    while time.time() < end:
        with state.ACK_LOCK:
            acked = {did for did, (ok, at) in state.RECENT_ACKS.items()
                     if at >= start_ack and ok}
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


def _clear_events(con, body):
    with state.EVENT_LOCK:
        state.EVENTS.clear()
    return {"ok": True, "msg": "monitor cleared"}


def _get_history(con, body):
    did = body.get("device_id", "")
    return {"ok": True, "history": sensor_history(did, int(body.get("limit", 288)))}


def _create_version(con, body):
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


def _list_versions(con, body):
    versions = [dict(r) for r in con.execute(
        "SELECT id,name,device_id,created_at FROM config_versions ORDER BY id DESC LIMIT 20")]
    return {"ok": True, "versions": versions}


def _diff_versions(con, body):
    a = body.get("a")
    b = body.get("b")
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
    try:
        da = json.loads(pa[0])
        db_ = json.loads(pb[0])
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


def _rollback_version(con, body):
    vid = body.get("id")
    if vid is None:
        return {"ok": False, "msg": "select a version"}
    row = con.execute("SELECT payload,device_id FROM config_versions WHERE id=?",
                     (int(vid),)).fetchone()
    if not row:
        return {"ok": False, "msg": "version not found"}
    payload, dev = row[0], row[1]
    # Atomic rollback: if replaying the version fails mid-way, undo the deletes
    # instead of leaving the config half-deleted/half-restored.
    try:
        con.isolation_level = None
        con.execute("BEGIN IMMEDIATE")
    except Exception:
        pass
    try:
        data = json.loads(payload)
    except Exception:
        data = None
    if isinstance(data, dict) and data.get("devices") is not None:
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
        try:
            con.isolation_level = ""
        except Exception:
            pass
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
    for tok in _re.findall(r"\S+", payload or ""):
        if tok.startswith("qos:p="):
            mm = _re.match(r"qos:p=(\d+):bw=(\d+):lat=(\d+):pre=(\d+)", tok)
            if mm:
                p, bw, lat, pre = map(int, mm.groups())
                con.execute("INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
                            "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?)",
                          (dev or "default", p, p, bw, lat, pre))
        elif tok.startswith("qos:"):
            mm = _re.match(r"qos:([^:]+):p=(\d+):bw=(\d+):lat=(\d+):pre=(\d+)", tok)
            if mm and len(mm.groups()) == 5:
                td, p, bw, lat, pre = mm.group(1), *map(int, mm.groups()[1:])
                con.execute("INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
                            "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?)",
                          (td, p, p, bw, lat, pre))
        elif tok.startswith("vlan:"):
            mm = _re.match(r"vlan:([^:]+):(\d+)", tok)
            if mm:
                con.execute("INSERT OR REPLACE INTO vlan_groups(id,name,vlan_id) "
                            "VALUES(?,?,?)", (mm.group(1), mm.group(1), int(mm.group(2))))
        elif tok.startswith("vmem:"):
            mm = _re.match(r"vmem:([^:]+):([^:]+)", tok)
            if mm:
                con.execute("INSERT OR IGNORE INTO vlan_members(group_id,device_id) "
                            "VALUES(?,?)", (mm.group(1), mm.group(2)))
        elif tok.startswith("stream:"):
            mm = _re.match(r"stream:([^:]+):([^:]+):vlan=(\d+):prio=(\d+):lat=(\d+):iv=(\d+):st=(\d+)", tok)
            if mm:
                sid, talker = mm.group(1), mm.group(2)
                vlan, prio, lat, iv, st = map(int, mm.groups()[2:])
                con.execute("INSERT OR REPLACE INTO tsn_streams(stream_id,name,talker,vlan_id,"
                           "max_latency_ns,max_interval_ns,priority,data_frame_prio,status,comment)"
                           " VALUES(?,?,?,?,?,?,?,?,?,'')",
                          (sid, sid, talker, vlan, lat, iv, prio, prio, st))
        elif tok.startswith("streammem:"):
            mm = _re.match(r"streammem:([^:]+):([^:]+)", tok)
            if mm:
                con.execute("INSERT OR IGNORE INTO tsn_stream_members(stream_id,role,device_id) "
                            "VALUES(?,'listener',?)", (mm.group(1), mm.group(2)))
        elif tok.startswith("tas:"):
            mm = _re.match(r"tas:([^:]+):cycle=(\d+):tgt=(\S+)", tok)
            if mm:
                con.execute("INSERT OR REPLACE INTO tas_schedules(id,name,cycle_time_ns,"
                           "deploy_target) VALUES(?,?,?,?)",
                          (mm.group(1), mm.group(1), int(mm.group(2)), mm.group(3)))
        elif tok.startswith("gcl:"):
            mm = _re.match(r"gcl:([^:]+):(\d+):(\d+):(\d+)", tok)
            if mm:
                con.execute("INSERT OR REPLACE INTO gcl_entries(schedule_id,index,gate_state,"
                           "duration_ns) VALUES(?,?,?,?)",
                          (mm.group(1), int(mm.group(2)), int(mm.group(3)), int(mm.group(4))))
    con.commit()
    try:
        con.isolation_level = ""
    except Exception:
        pass
    return {"ok": True, "msg": "rolled back to version " + str(vid)}


def _restore_backup(con, body):
    data = body.get("data", body)
    if not data or not isinstance(data, dict):
        return {"ok": False, "msg": "invalid backup payload"}
    tables = ["devices", "qos_configs", "preemption_configs", "vlan_groups",
              "vlan_members", "tas_schedules", "gcl_entries", "timesync_status",
              "tsn_streams", "tsn_stream_members", "settings"]
    # Atomic restore: a failed replay rolls everything back instead of leaving a
    # partially overwritten configuration.
    try:
        con.isolation_level = None
        con.execute("BEGIN IMMEDIATE")
    except Exception:
        pass
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
                    if _re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k):
                        cols.append("`" + k + "`")
                        vals.append(v)
                if not cols:
                    continue
                con.execute("INSERT INTO %s(%s) VALUES(%s)" %
                           (t, ",".join(cols), ",".join(["?"] * len(vals))), tuple(vals))
        con.commit()
    except Exception:
        try:
            con.execute("ROLLBACK")
        except Exception:
            pass
        raise
    try:
        con.isolation_level = ""
    except Exception:
        pass
    return {"ok": True, "msg": "configuration restored"}


HANDLERS = {
    "set_mode": _set_mode,
    "exec_all": _exec_all,
    "clear_events": _clear_events,
    "get_history": _get_history,
    "create_version": _create_version,
    "list_versions": _list_versions,
    "diff_versions": _diff_versions,
    "rollback_version": _rollback_version,
    "restore_backup": _restore_backup,
}
