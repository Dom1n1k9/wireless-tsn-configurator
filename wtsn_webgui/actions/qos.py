"""QoS (802.1Q) and preemption (802.1Qbu) actions."""
from ..db import add_event, clamp


def _save_qos(con, body):
    con.execute("INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
                "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?)",
                (body.get("device_id"), clamp(body.get("priority", 5), 0, 7),
                 clamp(body.get("traffic_class", 1), 0, 3),
                 clamp(body.get("bandwidth_kbps", 1000), 1, 1000000),
                 clamp(body.get("latency_ms", 1), 0, 10000),
                 clamp(body.get("preemption", 0), 0, 2)))
    con.commit()
    add_event("qos", "cnc", "802.1Q priority %s -> %s" % (body.get("priority"),
             body.get("device_id")), src_ip="", dst_ip="", dest=body.get("device_id"),
             proto="IEEE 802.1Q (QoS)")
    return {"ok": True, "msg": "QoS saved"}


def _delete_qos(con, body):
    con.execute("DELETE FROM qos_configs WHERE device_id=?", (body.get("device_id"),))
    con.commit()
    return {"ok": True, "msg": "QoS deleted"}


def _save_preemption(con, body):
    e = body.get("emac") or ""
    p = body.get("pmac") or ""
    if body.get("preemption", 0) == 0:
        e = p = ""
    con.execute("INSERT OR REPLACE INTO preemption_configs(device_id,preemption,"
                "emac,pmac) VALUES(?,?,?,?)",
                (body.get("device_id"), clamp(body.get("preemption", 0), 0, 1), e, p))
    con.commit()
    add_event("pre", "cnc", "802.1Qbu preemption %s -> %s (eMAC [%s] pMAC [%s])" %
             (body.get("device_id"), body.get("preemption"), e, p),
             src_ip="", dst_ip="", dest=body.get("device_id"), proto="IEEE 802.1Qbu")
    return {"ok": True, "msg": "Preemption saved"}


def _delete_preemption(con, body):
    con.execute("DELETE FROM preemption_configs WHERE device_id=?",
                (body.get("device_id"),))
    con.commit()
    return {"ok": True, "msg": "Preemption deleted"}


HANDLERS = {
    "save_qos": _save_qos,
    "delete_qos": _delete_qos,
    "save_preemption": _save_preemption,
    "delete_preemption": _delete_preemption,
}
