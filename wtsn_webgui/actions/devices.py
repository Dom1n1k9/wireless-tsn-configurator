"""Device-related actions: save/delete/reset devices, roles, ping, OTA."""
import json
import os
import time

from .. import state
from ..db import add_event, clamp, get_self_ip
from .. import mqtt_link


def _save_devices(con, body):
    for i in body.get("delete") or []:
        for t, c in (("devices", "id"), ("qos_configs", "device_id"),
                     ("vlan_members", "device_id"), ("sensors", "device_id"),
                     ("device_tsn_features", "device_id")):
            con.execute("DELETE FROM %s WHERE %s=?" % (t, c), (i,))
        with state.SIM_USER_DEVICES_LOCK:
            state.SIM_USER_DEVICES.discard(i)
        if state.MODE["mode"] == "real":
            b = mqtt_link.get_real_mqtt(con)
            if b:
                b.publish("tsn/cmd/%s/reset" % i, "{}")
        add_event("config", "cnc", "removed " + i)
    for i in body.get("reset") or []:
        if state.MODE["mode"] == "real":
            b = mqtt_link.get_real_mqtt(con)
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


def _set_role(con, body):
    role = body.get("role")
    did = body.get("id")
    if role == "grandmaster":
        con.execute("UPDATE timesync_status SET grandmaster=? WHERE id='main'", (did,))
        con.commit()
    add_event("config", "cnc", "%s -> %s" % (did, role))
    return {"ok": True, "msg": "role set"}


def _ping_device(con, body):
    did = body.get("id", "")
    if not did:
        return {"ok": False, "msg": "missing device id"}
    b = mqtt_link.get_real_mqtt(con) if state.MODE["mode"] == "real" else None
    if not b:
        add_event("config", "cnc", "identify %s (no broker)" % did)
        return {"ok": False, "msg": "no broker in real mode"}
    b.publish("tsn/cmd/%s/ping" % did, "1")
    # Remember when the ping went out so the ack can be timestamped into an RTT
    # latency sample for the TSN Metrics page.
    state.PING_OUT[did] = time.time()
    cnc_ip = get_self_ip()
    add_event("mqtt", "cnc", "PING -> %s" % did, src_ip=cnc_ip, dst_ip="",
              dest=did, proto="MQTT")
    return {"ok": True, "msg": "ping sent to " + did}


def _start_ota(con, body):
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
    if host in ("0.0.0.0", "::"):
        host = "127.0.0.1"
    b = mqtt_link.get_real_mqtt(con) if state.MODE["mode"] == "real" else None
    url = "http://%s:%d/fw/%s" % (host, state.PORT, fname)
    if not b:
        add_event("ota", "cnc", "OTA %s <- %s (no broker, simulated)" % (did, fname))
        return {"ok": True, "msg": "OTA requested (simulated) for " + did}
    b.publish("tsn/cmd/%s/ota" % did, json.dumps({"url": url}))
    add_event("ota", "cnc", "OTA %s <- %s (%s)" % (did, fname, url))
    return {"ok": True, "msg": "OTA started on " + did}


HANDLERS = {
    "save_devices": _save_devices,
    "set_role": _set_role,
    "ping_device": _ping_device,
    "start_ota": _start_ota,
}
