"""FX / OPC UA FX over MQTT (FXMQTT) actions."""
from .. import state
from ..db import add_event
from .. import mqtt_link


def _fx_send(con, body):
    b = mqtt_link.get_real_mqtt(con) if state.MODE["mode"] == "real" else None
    src = body.get("source", "cnc") or "cnc"
    add_event("fx", src, ("tsn/fx/cmd/%s <- " % src) + body.get("msg", ""))
    if b:
        b.publish("tsn/fx/cmd/%s" % src, body.get("msg", ""))
        return {"ok": True, "msg": "FX published on broker"}
    return {"ok": True, "msg": "FX sent (no broker / simulation)"}


HANDLERS = {
    "fx_send": _fx_send,
}
