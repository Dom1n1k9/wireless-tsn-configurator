"""802.1Qcc stream reservation + WiFi + FX send actions."""
import json

from .. import state
from ..db import add_event, clamp
from .. import mqtt_link
from .core import deploy_stream_msg, stream_payload


def _set_wifi(con, body):
    ssid = body.get("ssid", "")
    pw = body.get("pass", "")
    if state.MODE["mode"] == "real":
        b = mqtt_link.get_real_mqtt(con)
        if b:
            did = body.get("device_id", "")
            if did:
                b.publish("tsn/cmd/%s/wifi" % did, json.dumps({"ssid": ssid, "pass": pw}))
                add_event("config", "cnc", "wifi cmd -> %s (%s)" % (did, ssid))
                return {"ok": True, "msg": "WiFi command sent to " + did}
            return {"ok": False, "msg": "device_id required"}
        return {"ok": False, "msg": "MQTT broker not reachable"}
    return {"ok": True, "msg": "WiFi set (simulation; sent on Real mode)"}


def _save_stream(con, body):
    sid = body.get("stream_id") or ("stream-%d" % __import__("time").time())
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


def _delete_stream(con, body):
    sid = body.get("stream_id", "")
    con.execute("DELETE FROM tsn_streams WHERE stream_id=?", (sid,))
    con.execute("DELETE FROM tsn_stream_members WHERE stream_id=?", (sid,))
    con.commit()
    return {"ok": True, "msg": "Stream deleted"}


def _deploy_stream(con, body):
    sid = body.get("stream_id", "")
    con.execute("UPDATE tsn_streams SET status=1 WHERE stream_id=?", (sid,))
    con.commit()
    if state.MODE["mode"] == "real":
        broker = mqtt_link.get_real_mqtt(con)
        if broker:
            payload = stream_payload(con, sid)
            if payload:
                deploy_stream_msg(broker, con, sid, payload)
        else:
            return {"ok": False, "msg": "MQTT broker not reachable"}
    add_event("config", "cnc", "802.1Qcc stream %s deployed via FXMQTT" % sid)
    return {"ok": True, "msg": "Stream deployed via FXMQTT"}


def _deploy_all_streams(con, body):
    con.execute("UPDATE tsn_streams SET status=1")
    con.commit()
    n = con.execute("SELECT COUNT(*) FROM tsn_streams").fetchone()[0]
    if state.MODE["mode"] == "real":
        broker = mqtt_link.get_real_mqtt(con)
        if broker:
            for sr in con.execute("SELECT * FROM tsn_streams").fetchall():
                payload = stream_payload(con, sr["stream_id"])
                if payload:
                    deploy_stream_msg(broker, con, sr["stream_id"], payload)
        else:
            return {"ok": False, "msg": "MQTT broker not reachable"}
    add_event("config", "cnc", "802.1Qcc all %d streams deployed via FXMQTT" % n)
    return {"ok": True, "msg": "%d streams deployed via FXMQTT" % n}


HANDLERS = {
    "set_wifi": _set_wifi,
    "save_stream": _save_stream,
    "delete_stream": _delete_stream,
    "deploy_stream": _deploy_stream,
    "deploy_all_streams": _deploy_all_streams,
}
