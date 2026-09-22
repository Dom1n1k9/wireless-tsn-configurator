"""Device-related actions: save/delete/reset devices, roles, ping, OTA."""
import json
import os
import random
import re
import threading
import time

from .. import state
from ..db import add_event, clamp, connect, crc32_hex, get_self_ip
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
        fields = ["name", "ip", "mac", "kind", "firmware", "status", "domain", "usb"]
        vals = [dev.get("name", ""), dev.get("ip", ""), dev.get("mac", ""),
                clamp(dev.get("kind", 0), 0, 3), dev.get("firmware", ""),
                clamp(dev.get("status", 0), 0, 2), dev.get("domain", "default"),
                dev.get("usb", "")]
        existing = con.execute("SELECT id,usb FROM devices WHERE id=?",
                               (dev["id"],)).fetchone()
        if existing:
            if not dev.get("usb") and existing[1]:
                vals[7] = existing[1]  # keep the serial port when not re-sent
            sets = ",".join("%s=?" % f for f in fields)
            con.execute("UPDATE devices SET %s,last_seen=strftime('%%s','now') WHERE id=?" % sets,
                        vals + [dev["id"]])
        else:
            con.execute("INSERT INTO devices(id,name,ip,mac,kind,firmware,status,"
                        "last_seen,domain,usb) VALUES(?,?,?,?,?,?,?,strftime('%s','now'),?,?)",
                        [dev["id"]] + vals)
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
    cnc_ip = get_self_ip()
    # Resolve the canonical UI id ('esp32-cam') back to the real DB row (the
    # cam may be stored under esp32-cam / esp32-cam-01 / whatever announced).
    row = con.execute("SELECT id,ip,kind FROM devices WHERE id=?", (did,)).fetchone()
    if row is None:
        for r in con.execute("SELECT id,ip,kind FROM devices"):
            if re.search(r"cam", r["id"], re.I) or int(r["kind"] or 0) == 5:
                row = r
                break
    is_cam = bool(row and (int(row["kind"] or 0) == 5 or re.search(r"cam", row["id"], re.I)))

    # A real ESP32-CAM never answers the MQTT /ping command (the cam firmware
    # only handles /ota + motion feeds). Instead we probe the camera's own HTTP
    # server: a reachable camera = online. This also yields a real round-trip
    # sample for the TSN Metrics page.
    if is_cam:
        import urllib.request
        real_id, cam_ip = row["id"], row["ip"] or ""
        t0 = time.time()
        status, note = "offline", "no ip"
        if cam_ip:
            try:
                url = "http://%s/last.jpg" % cam_ip
                req = urllib.request.Request(url, headers={"User-Agent": "wtsn-gui"})
                with urllib.request.urlopen(req, timeout=2.0) as r:
                    r.read(64)
                status, note = "online", "HTTP OK (%s)" % url
            except Exception as ex:  # noqa: BLE001
                status, note = "offline", "%s (%s)" % (ex.__class__.__name__, ex)
        rtt_ms = (time.time() - t0) * 1000.0
        add_event("mqtt", "cnc",
                  "PING cam %s -> %s: %s (%.1f ms)" % (did, cam_ip or "?", note, rtt_ms),
                  src_ip=cnc_ip, dst_ip=cam_ip, dest=did, proto="HTTP")
        state.PING_OUT[did] = time.time()
        try:
            con.execute("INSERT INTO latency_log(device_id,ts,latency_ms) "
                        "VALUES(?,?,?)", (real_id, int(time.time()), round(rtt_ms, 2)))
            con.commit()
        except Exception:  # noqa: BLE001
            pass
        if status == "online":
            try:
                con.execute("UPDATE devices SET status=0,last_seen=strftime('%s','now') "
                            "WHERE id=?", (real_id,))
                con.commit()
            except Exception:  # noqa: BLE001
                pass
        return {"ok": status == "online",
                "msg": "cam %s is %s (%s, %.1f ms)" % (did, status, note, rtt_ms)}

    add_event("mqtt", "cnc", "PING -> %s" % did, src_ip=cnc_ip, dst_ip="",
              dest=did, proto="MQTT")
    # Remember when the ping went out so the ack can be timestamped into an RTT
    # latency sample for the TSN Metrics page.
    state.PING_OUT[did] = time.time()
    if state.MODE["mode"] != "real":
        # No real agent to answer in simulation: reply after a realistic RTT and
        # record the round trip so the Metrics latency chart shows the sample.
        def _sim_ping_ack():
            c = connect()
            try:
                mqtt_link.record_round_trip(c, did)
            finally:
                c.close()
            add_event("mqtt", did, "PING ack %s (simulated)" % did, src_ip=cnc_ip,
                      dst_ip="", dest="cnc", proto="MQTT")
            state.WS_NOTIFY.set()
        threading.Timer(random.uniform(0.005, 0.045), _sim_ping_ack).start()
        return {"ok": True, "msg": "ping sent to " + did}
    b = mqtt_link.get_real_mqtt(con)
    if not b:
        add_event("config", "cnc", "identify %s (no broker)" % did)
        return {"ok": False, "msg": "no broker in real mode"}
    b.publish("tsn/cmd/%s/ping" % did, "1")
    return {"ok": True, "msg": "ping sent to " + did}


def _start_ota(con, body):
    did = body.get("id", "")
    fname = body.get("file", "")
    if not did:
        return {"ok": False, "msg": "missing device id"}
    dev = con.execute("SELECT kind,firmware FROM devices WHERE id=?", (did,)).fetchone()
    if not dev:
        return {"ok": False, "msg": "unknown device " + did}
    devkind = int(dev[0] or 0)
    if not fname:
        row = con.execute("SELECT file FROM firmware WHERE kind IN (?,-1)"
                          " ORDER BY uploaded_at DESC, file DESC LIMIT 1", (devkind,)).fetchone()
        fname = row[0] if row else ""
        if not fname:
            try:
                files = sorted((f for f in os.listdir(state.FW_DIR)
                                if f.endswith((".bin", ".img", ".hex"))),
                               key=lambda f: os.path.getmtime(os.path.join(state.FW_DIR, f)),
                               reverse=True)
            except OSError:
                files = []
            fname = files[0] if files else ""
    if not fname or not os.path.isfile(os.path.join(state.FW_DIR, fname)):
        return {"ok": False, "msg": "no firmware stored — upload a .bin/.img/.hex first"}
    meta = con.execute("SELECT version,crc32,kind FROM firmware WHERE file=?",
                       (fname,)).fetchone()
    fwkind = int(meta[2]) if meta and meta[2] is not None else -1
    if fwkind != -1 and fwkind != devkind:
        return {"ok": False,
                "msg": "%s is for kind %d, device %s is kind %d — not compatible"
                       % (fname, fwkind, did, devkind)}
    version = (meta[0] if meta else "") or ""
    stored_crc = (meta[1] if meta else "") or ""
    # The devices pull the firmware over HTTP from the CNC. WTSN_HOST defaults
    # to 127.0.0.1 (GUI binds localhost), which an ESP could never reach — so
    # when the env has no explicit LAN address we fall back to the CNC's actual
    # LAN IP (same value the ping source uses). This keeps OTA working when the
    # CNC runs on the RPi instead of this PC.
    host = (os.environ.get("WTSN_HOST") or "").strip()
    if host in ("0.0.0.0", "::", "127.0.0.1", "localhost", ""):
        host = get_self_ip()
    b = mqtt_link.get_real_mqtt(con) if state.MODE["mode"] == "real" else None
    url = "http://%s:%d/fw/%s" % (host, state.PORT, fname)
    if not b:
        # Simulated device: download the image, verify its CRC exactly like a
        # real node would, flash, then report the new running version.
        with open(os.path.join(state.FW_DIR, fname), "rb") as f:
            data = f.read()
        real_crc = crc32_hex(data)
        if stored_crc and real_crc != stored_crc:
            add_event("ota", "cnc", "OTA %s <- %s ABORTED (crc %s != stored %s)"
                      % (did, fname, real_crc, stored_crc))
            return {"ok": False, "msg": "CRC32 mismatch — image corrupted, flash aborted"}
        newver = version or fname
        con.execute("UPDATE devices SET firmware=? WHERE id=?", (newver, did))
        con.commit()
        add_event("ota", "cnc", "OTA %s <- %s (simulated flash, crc32 %s verified) -> %s"
                  % (did, fname, real_crc, newver))
        return {"ok": True,
                "msg": "flashed %s on %s — crc32 %s verified, now running %s"
                       % (fname, did, real_crc, newver)}
    b.publish("tsn/cmd/%s/ota" % did,
              json.dumps({"url": url, "crc32": stored_crc, "version": version}))
    add_event("ota", "cnc", "OTA %s <- %s (%s, crc32 %s)" % (did, fname, url, stored_crc))
    return {"ok": True,
            "msg": "OTA started on %s — device downloads %s and verifies crc32 %s"
                   % (did, fname, stored_crc)}


HANDLERS = {
    "save_devices": _save_devices,
    "set_role": _set_role,
    "ping_device": _ping_device,
    "start_ota": _start_ota,
}
