"""Time synchronization (802.1AS gPTP) + set_server actions."""
import time

from ..db import add_event


def _set_server(con, body):
    server_type = body.get("type", "node")
    did = body.get("id", "")
    broker = body.get("broker", "127.0.0.1:1883")
    con.execute("INSERT OR REPLACE INTO settings(key,value) VALUES('server_type',?)",
                (server_type,))
    con.execute("INSERT OR REPLACE INTO settings(key,value) VALUES('server_id',?)", (did,))
    con.execute("INSERT OR REPLACE INTO settings(key,value) VALUES('broker',?)", (broker,))
    con.commit()
    add_event("config", "cnc",
              "server = " + ("PC" if server_type == "pc" else "node " + did))
    return {"ok": True, "msg": ("server = PC" if server_type == "pc"
                                else "server = node " + did)}


def _save_timesync(con, body):
    mode = int(body.get("mode", 0))
    gm = body.get("grandmaster", "")
    nodes = body.get("nodes") or []
    sid = body.get("id", "main")
    con.execute("INSERT OR REPLACE INTO timesync_status(id,mode,grandmaster,offset_ns,quality) "
                "VALUES(?,?,?,?,?)", (sid, mode, gm, 0, 1))
    con.commit()
    add_event("timesync", "cnc", "802.1AS mode=%d gm=%s slaves=%s" %
             (mode, gm or "-", ",".join(nodes) or "-"),
             src_ip="", dst_ip="", dest=gm, proto="IEEE 802.1AS")
    return {"ok": True, "msg": "time sync saved"}


def _sync_report(con, body):
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


HANDLERS = {
    "set_server": _set_server,
    "save_timesync": _save_timesync,
    "sync_report": _sync_report,
}
