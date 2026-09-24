"""FX / OPC UA FX over MQTT (FXMQTT) actions."""
from .. import state
from ..db import add_event
from .. import mqtt_link


def _fx_send(con, body):
    src = body.get("source", "cnc") or "cnc"
    msg = body.get("msg", "")
    add_event("fx", src, ("tsn/fx/cmd/%s <- " % src) + msg)
    if state.MODE["mode"] == "real":
        b = mqtt_link.get_real_mqtt(con)
        if b:
            b.publish("tsn/fx/cmd/%s" % src, msg)
            return {"ok": True, "msg": "FX published on broker"}
        return {"ok": False, "msg": "MQTT broker not reachable"}
    # Simulation: the value appears in the live FX data view like a real
    # field-server sample would.
    con.execute("INSERT INTO fx_data(ts,src,data_id,value,text)"
                " VALUES(strftime('%s','now'),?,?,?,?)",
                (src, "manual", 0, msg))
    con.commit()
    state.WS_NOTIFY.set()
    return {"ok": True, "msg": "FX sent (simulation) — visible in Live FX data"}


def _send_mqtt(con, body):
    """Publish a raw message on any MQTT topic (used by the SSD1306 Display
    dialog and other ad-hoc node commands)."""
    topic = (body.get("topic") or "").strip()
    payload = body.get("payload", "")
    if not topic:
        return {"ok": False, "msg": "missing topic"}
    topic = str(topic)[:160]
    add_event("mqtt", "cnc", "send -> %s <- %s" % (topic, str(payload)[:80]))
    if state.MODE["mode"] == "real":
        b = mqtt_link.get_real_mqtt(con)
        if not b:
            return {"ok": False, "msg": "MQTT broker not reachable"}
        b.publish(topic, payload)
        return {"ok": True, "msg": "published on " + topic}
    return {"ok": True, "msg": "(simulation) would publish " + topic}


def _fx_recent(con, body):
    rows = con.execute("SELECT ts,src,data_id,value,text FROM fx_data"
                       " ORDER BY id DESC LIMIT 200").fetchall()
    return {"ok": True,
            "data": [dict(r) for r in reversed(rows)]}


HANDLERS = {
    "fx_send": _fx_send,
    "fx_recent": _fx_recent,
    "send_mqtt": _send_mqtt,
}
