"""WTSN AI Policy Engine (runs on the Raspberry Pi edge node).

Deterministic decision layer that watches live telemetry from the web GUI
(sensors, metrics, time-sync state) and reconfigures the TSN domain through
the normal action API, so every change is visible in the GUI and recorded
in the AI Decisions audit trail with provenance (source="ai").

Active rules:
  R1  person (AI camera) + PIR motion  -> raise QoS priority + open TAS gate
      for that device, deploy. Restores previous QoS when calm for 5 min.
  R2  gPTP grandmaster |offset| too high -> switch grandmaster to the
      best-offset node.
  R3  E2E control-plane latency too high for a device -> reserve an
      802.1Qcc stream for it (talker -> grandmaster).

Config file JSON, default /home/wtsn/wtsn-ai/policy.json (all keys optional):
  {"gui": "http://127.0.0.1:8000",
   "poll_s": 5,
   "r1": {"cooldown_s": 120, "calm_s": 300, "pir_window_s": 60,
          "priority": 5, "traffic_class": 3, "bandwidth_kbps": 50000},
   "r2": {"offset_ns": 500, "cooldown_s": 300},
   "r3": {"latency_ms": 50, "min_samples": 3, "cooldown_s": 300,
          "vlan_id": 100, "priority": 5}}
"""
import json
import os
import time
import urllib.request

BASE = {
    "gui": os.environ.get("WTSN_AI_GUI", "http://127.0.0.1:8000"),
    "poll_s": 5,
    "r1": {"cooldown_s": 120, "calm_s": 300, "pir_window_s": 60,
           "priority": 5, "traffic_class": 3, "bandwidth_kbps": 50000,
           "latency_ms": 5, "cycle_ns": 4000000},
    "r2": {"offset_ns": 500, "cooldown_s": 300, "min_samples": 3},
    "r3": {"latency_ms": 50, "min_samples": 3, "cooldown_s": 300,
           "vlan_id": 100, "priority": 5, "max_latency_ns": 2000000,
           "max_interval_ns": 200000},
}
STATE_PATH = os.path.join(os.path.expanduser("~"), "wtsn-ai", "policy_state.json")


def log(msg):
    print("[wtsn-policy] %s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def load_cfg():
    path = os.environ.get("WTSN_AI_POLICY",
                          os.path.join(os.path.expanduser("~"), "wtsn-ai", "policy.json"))
    if os.path.isfile(path):
        try:
            data = json.load(open(path))
            for k, v in data.items():
                if isinstance(v, dict) and isinstance(BASE.get(k), dict):
                    BASE[k].update(v)
                else:
                    BASE[k] = v
        except Exception as ex:  # noqa: BLE001
            log("bad policy config %s: %s" % (path, ex))
    return BASE


def http_json(url, body=None):
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers,
                                 method="POST" if body is not None else "GET")
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def action(name, body):
    body = dict(body)
    body.setdefault("source", "ai")
    try:
        res = http_json(BASE["gui"] + "/api/actions/" + name, body)
        if not res.get("ok"):
            log("action %s failed: %s" % (name, res.get("msg")))
        return res
    except Exception as ex:  # noqa: BLE001
        log("action %s error: %s" % (name, ex))
        return {"ok": False, "msg": str(ex)}


def load_state():
    try:
        return json.load(open(STATE_PATH))
    except Exception:  # noqa: BLE001
        return {}


def save_state(st):
    os.makedirs(os.path.dirname(STATE_PATH), exist_ok=True)
    tmp = STATE_PATH + ".tmp"
    json.dump(st, open(tmp, "w"))
    os.replace(tmp, STATE_PATH)


def sensor_val(data, did, sid):
    for s in data.get("sensors", []):
        if s.get("device_id") == did and s.get("sensor_id") == sid:
            return s.get("value")
    return None


def rule_r1(data, st, now):
    """person (AI camera) + PIR -> QoS boost + TAS gate on that device."""
    c = BASE["r1"]
    cams = [d["id"] for d in data.get("devices", [])
            if sensor_val(data, d["id"], "ai_person") == 1]
    pir_recent = [d["id"] for d in data.get("devices", [])
                  if sensor_val(data, d["id"], "pir1") == 1 and
                  (now - (d.get("last_seen") or 0)) < c["pir_window_s"]]
    for cam in cams:
        s = st.setdefault(cam, {})
        if cams and pir_recent and not s.get("boosted"):
            if s.get("last_fire", 0) + c["cooldown_s"] > now:
                continue
            prev = {}
            for q in data.get("qos_configs", []):
                if q.get("device_id") == cam:
                    prev = {"priority": q.get("priority", 3),
                            "traffic_class": q.get("traffic_class", 1),
                            "bandwidth_kbps": q.get("bandwidth_kbps", 1000),
                            "latency_ms": q.get("latency_ms", 1)}
            action("create_version", {"name": "ai-pre-%s" % cam,
                                      "device_id": cam,
                                      "reason": "snapshot before AI QoS/TAS boost"})
            action("save_qos", {"device_id": cam,
                                "priority": c["priority"],
                                "traffic_class": c["traffic_class"],
                                "bandwidth_kbps": c["bandwidth_kbps"],
                                "latency_ms": c["latency_ms"],
                                "reason": "AI: person (camera) + PIR motion - raising priority to %d" % c["priority"]})
            action("save_tas", {"id": "ai-gate-%s" % cam,
                                "name": "AI motion gate",
                                "cycle_time_ns": c["cycle_ns"],
                                "deploy_target": cam,
                                "gcl": [{"gate_state": 1 << c["priority"],
                                         "duration_ns": c["cycle_ns"] // 2},
                                        {"gate_state": 0xFF,
                                         "duration_ns": c["cycle_ns"] // 2}],
                                "reason": "AI: opening protected gate window for priority %d traffic" % c["priority"]})
            action("exec_all", {"reason": "AI: deploy QoS+TAS for person+PIR event"})
            s.update({"boosted": True, "last_fire": now, "prev": prev,
                      "boost_at": now})
            log("R1: %s boosted (person+PIR)" % cam)
        elif not cams and s.get("boosted"):
            if now - s.get("boost_at", now) >= c["calm_s"]:
                p = s.get("prev") or {}
                action("save_qos", {"device_id": cam,
                                    "priority": p.get("priority", 3),
                                    "traffic_class": p.get("traffic_class", 1),
                                    "bandwidth_kbps": p.get("bandwidth_kbps", 1000),
                                    "latency_ms": p.get("latency_ms", 1),
                                    "reason": "AI: calm restored - reverting QoS to priority %s" % p.get("priority", 3)})
                action("delete_tas", {"id": "ai-gate-%s" % cam,
                                      "reason": "AI: removing motion gate"})
                action("exec_all", {"reason": "AI: deploy reverted QoS"})
                s.pop("boosted", None)
                log("R1: %s reverted (calm)" % cam)


def rule_r2(data, st, now):
    """gPTP: grandmaster offset too high -> switch to best node."""
    c = BASE["r2"]
    try:
        m = http_json(BASE["gui"] + "/api/actions/metrics",
                      {"window": 300, "window2": 600})
    except Exception:  # noqa: BLE001
        return
    if not m.get("ok"):
        return
    tssum = {r["device_id"]: r for r in m.get("timesync", {}).get("summary", [])}
    gm_row = (data.get("timesync_status") or [{}])[0]
    gm = gm_row.get("grandmaster", "")
    if not gm or gm not in tssum:
        return
    if st.get("r2_at", 0) + c["cooldown_s"] > now:
        return
    if abs(tssum[gm].get("avg") or 0) < c["offset_ns"]:
        return
    cands = [(abs(r.get("avg") or 10**9), did) for did, r in tssum.items()
             if did != gm and r.get("n", 0) >= c["min_samples"]]
    if not cands:
        return
    cands.sort()
    best = cands[0][1]
    nodes = [d["id"] for d in data.get("devices", []) if d["id"] != best]
    action("save_timesync", {"mode": 1, "grandmaster": best, "nodes": nodes,
                             "reason": "AI: GM %s offset %dns > %dns threshold - switching to %s"
                             % (gm, int(abs(tssum[gm].get("avg") or 0)),
                                c["offset_ns"], best)})
    st["r2_at"] = now
    log("R2: grandmaster %s -> %s" % (gm, best))


def rule_r3(data, st, now):
    """latency too high for a device -> reserve an 802.1Qcc stream for it."""
    c = BASE["r3"]
    try:
        m = http_json(BASE["gui"] + "/api/actions/metrics",
                      {"window": 300, "window2": 600})
    except Exception:  # noqa: BLE001
        return
    if not m.get("ok"):
        return
    gm_row = (data.get("timesync_status") or [{}])[0]
    gm = gm_row.get("grandmaster") or "esp32-01"
    talkers = {s.get("talker") for s in data.get("tsn_streams", [])}
    for r in m.get("latency", {}).get("summary", []):
        did = r.get("device_id", "")
        if not did or r.get("n", 0) < c["min_samples"]:
            continue
        if (r.get("avg") or 0) < c["latency_ms"]:
            continue
        if did in talkers:
            continue
        if st.get("r3_" + did, 0) + c["cooldown_s"] > now:
            continue
        sid = "ai-stream-%s" % did
        action("save_stream", {"stream_id": sid, "name": "AI reserve %s" % did,
                               "talker": did, "listeners": [gm],
                               "vlan_id": c["vlan_id"],
                               "max_latency_ns": c["max_latency_ns"],
                               "max_interval_ns": c["max_interval_ns"],
                               "priority": c["priority"],
                               "data_frame_prio": c["priority"],
                               "comment": "reserved by AI policy",
                               "reason": "AI: E2E latency %sms > %sms - reserving stream to %s"
                               % (int(r["avg"]), c["latency_ms"], gm)})
        action("deploy_stream", {"stream_id": sid,
                                 "reason": "AI: deploy reserved stream"})
        st["r3_" + did] = now
        log("R3: stream reserved for %s (latency %sms)" % (did, int(r["avg"])))


def tick(st):
    now = int(time.time())
    try:
        data = http_json(BASE["gui"] + "/api/data")
    except Exception as ex:  # noqa: BLE001
        log("GUI unreachable: %s" % ex)
        return
    rule_r1(data, st, now)
    rule_r2(data, st, now)
    rule_r3(data, st, now)
    save_state(st)


def main():
    load_cfg()
    st = load_state()
    log("policy engine started (gui=%s)" % BASE["gui"])
    while True:
        t0 = time.time()
        try:
            tick(st)
        except Exception as ex:  # noqa: BLE001
            log("tick error: %s" % ex)
        time.sleep(max(1.0, BASE["poll_s"] - (time.time() - t0)))


if __name__ == "__main__":
    main()
