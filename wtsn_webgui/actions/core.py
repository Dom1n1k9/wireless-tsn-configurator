"""Shared helpers for the per-domain action modules."""
import json


def stream_payload(con, sid):
    """Build the 802.1Qcc stream JSON snapshot sent to endpoints."""
    sr = con.execute("SELECT * FROM tsn_streams WHERE stream_id=?", (sid,)).fetchone()
    if not sr:
        return None
    memb = con.execute("SELECT role,device_id FROM tsn_stream_members WHERE stream_id=?",
                       (sid,)).fetchall()
    talker = next((m["device_id"] for m in memb if m["role"] == "talker"), "")
    listeners = [m["device_id"] for m in memb if m["role"] == "listener"]
    return json.dumps({"stream_id": sid, "name": sr["name"], "talker": talker,
                       "listeners": listeners, "vlan_id": sr["vlan_id"],
                       "max_latency_ns": sr["max_latency_ns"],
                       "max_interval_ns": sr["max_interval_ns"],
                       "priority": sr["priority"],
                       "data_frame_prio": sr["data_frame_prio"]})


def deploy_stream_msg(broker, con, sid, payload):
    """Publish the stream snapshot to its talker/listeners (FX command channel).
    Returns the number of messages published."""
    memb = con.execute("SELECT role,device_id FROM tsn_stream_members WHERE stream_id=?",
                       (sid,)).fetchall()
    talker = next((m["device_id"] for m in memb if m["role"] == "talker"), "")
    n = 0
    if talker:
        broker.publish("tsn/fx/cmd/%s" % talker, payload)
        broker.publish("tsn/cmd/%s/stream" % talker, payload)
        n += 1
    for l in [m["device_id"] for m in memb if m["role"] == "listener"]:
        broker.publish("tsn/cmd/%s/stream" % l, payload)
        n += 1
    return n
