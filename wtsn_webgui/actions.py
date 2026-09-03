"""Action handlers behind the /api/action and /api/actions/<name> endpoints."""
import json
import os
import re as _re
import time

from . import state
from .db import add_event, clamp, connect, get_self_ip
from .mqtt_link import get_real_mqtt


def run_action(act, body):
    con = connect()
    try:
        if act == "set_mode":
            m = body.get("mode")
            if m not in ("sim", "real"):
                return {"ok": False, "msg": "mode must be 'sim' or 'real'"}
            state.MODE["mode"] = m
            state.EVENTS.clear()
            add_event("config", "cnc",
                      "mode = SIMULATION" if m == "sim" else "mode = REAL (waiting for real devices)")
            return {"ok": True, "msg": "mode = " + m}
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
            did = body.get("device_id")
            dom = body.get("domain")
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
                with state.SIM_USER_DEVICES_LOCK:
                    state.SIM_USER_DEVICES.discard(i)
                if state.MODE["mode"] == "real":
                    b = get_real_mqtt(con)
                    if b:
                        b.publish("tsn/cmd/%s/reset" % i, "{}")
                add_event("config", "cnc", "removed " + i)
            for i in body.get("reset") or []:
                if state.MODE["mode"] == "real":
                    b = get_real_mqtt(con)
                    if b:
                        b.publish("tsn/cmd/%s/reset" % i, "{}")
                        add_event("config", "cnc", "reset issued -> %s" % i)
                else:
                    add_event("config", "cnc", "reset (simulation) -> %s" % i)
            dev = body.get("device") or {}
            if dev.get("id"):
                con.execute("INSERT OR REPLACE INTO devices(id,name,ip,mac,kind,firmware,status,"
                            "last_seen,domain) VALUES(?,?,?,?,?,?,?,strftime('%s','now'),?)",
                            (dev["id"], dev.get("name", ""), dev.get("ip", ""),
                             dev.get("mac", ""), clamp(dev.get("kind", 0), 0, 3),
                             dev.get("firmware", ""), clamp(dev.get("status", 0), 0, 2),
                             dev.get("domain", "default")))
                con.execute("DELETE FROM device_tsn_features WHERE device_id=?", (dev["id"],))
                for f in dev.get("tsn") or []:
                    con.execute("INSERT INTO device_tsn_features(device_id,feature) VALUES(?,?)",
                                (dev["id"], f))
                if state.MODE["mode"] == "sim":
                    with state.SIM_USER_DEVICES_LOCK:
                        state.SIM_USER_DEVICES.add(dev["id"])
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
                    if m["role"] == "talker":
                        talker = m["device_id"]
                    elif m["role"] == "listener":
                        listeners.append(m["device_id"])
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
                with state.ACK_LOCK:
                    acked = set(state.RECENT_ACKS.keys())
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
            if state.MODE["mode"] == "real":
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
            if state.MODE["mode"] == "real":
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
            if state.MODE["mode"] == "real":
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
            b = get_real_mqtt(con) if state.MODE["mode"] == "real" else None
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
            b = get_real_mqtt(con) if state.MODE["mode"] == "real" else None
            if not b:
                add_event("config", "cnc", "identify %s (no broker)" % did)
                return {"ok": False, "msg": "no broker in real mode"}
            b.publish("tsn/cmd/%s/ping" % did, "1")
            cnc_ip = get_self_ip()
            add_event("mqtt", "cnc", "PING -> %s" % did, src_ip=cnc_ip, dst_ip="",
                      dest=did, proto="MQTT")
            return {"ok": True, "msg": "ping sent to " + did}
        if act == "clear_events":
            state.EVENTS.clear()
            return {"ok": True, "msg": "monitor cleared"}
        if act == "get_history":
            from .db import sensor_history
            did = body.get("device_id", "")
            return {"ok": True, "history": sensor_history(did, int(body.get("limit", 288)))}
        if act == "start_ota":
            did = body.get("id", "")
            fname = body.get("file", "")
            if not did:
                return {"ok": False, "msg": "missing device id"}
            if not fname:
                try:
                    files = sorted((f for f in os.listdir(state.FW_DIR)
                                    if f.endswith((".bin", ".img"))),
                                   key=lambda f: os.path.getmtime(os.path.join(state.FW_DIR, f)),
                                   reverse=True)
                except OSError:
                    files = []
                fname = files[0] if files else ""
            if not fname or not os.path.isfile(os.path.join(state.FW_DIR, fname)):
                return {"ok": False, "msg": "no firmware uploaded (pick a .bin on the Devices page)"}
            host = os.environ.get("WTSN_HOST", "127.0.0.1")
            host = get_self_ip() if host in ("0.0.0.0", "127.0.0.1") else host
            url = "http://%s:%d/fw/%s" % (host, state.PORT, fname)
            if state.MODE["mode"] == "real":
                b = get_real_mqtt(con)
                if not b:
                    return {"ok": False, "msg": "MQTT broker not reachable"}
                size = os.path.getsize(os.path.join(state.FW_DIR, fname))
                b.publish("tsn/cmd/%s/ota" % did,
                          json.dumps({"url": url, "size": size}))
                add_event("config", "cnc", "OTA %s -> %s (%d B)" % (fname, did, size))
                return {"ok": True, "msg": "OTA sent to %s: %s" % (did, url)}
            add_event("config", "cnc", "OTA (simulation) %s -> %s" % (fname, did))
            return {"ok": True, "msg": "OTA simulated: %s" % url}
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
            # attempt a coarse field-level diff for JSON payloads
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
    except Exception as ex:
        return {"ok": False, "msg": str(ex)}
    finally:
        con.close()
    return {"ok": False, "msg": "unknown action: %s" % act}
