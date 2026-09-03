"""TAS / GCL (802.1Qbv) actions."""
from ..db import add_event


def _save_tas(con, body):
    sid = body.get("id") or ("tas-%d" % int(__import__("time").time()))
    con.execute("INSERT OR REPLACE INTO tas_schedules(id,name,cycle_time_ns,deploy_target) "
                "VALUES(?,?,?,?)",
                (sid, body.get("name", ""), int(body.get("cycle_time_ns", 1000000)),
                 body.get("deploy_target", "")))
    con.execute("DELETE FROM gcl_entries WHERE schedule_id=?", (sid,))
    for i, g in enumerate(body.get("gcl") or []):
        con.execute("INSERT INTO gcl_entries(schedule_id,\"index\",gate_state,duration_ns) "
                    "VALUES(?,?,?,?)",
                    (sid, i, int(g.get("gate_state", 0)), int(g.get("duration_ns", 0))))
    con.commit()
    add_event("tas", "cnc", "TAS schedule %s (cycle %s ns, %d gates)" %
             (sid, body.get("cycle_time_ns"), len(body.get("gcl") or [])),
             src_ip="", dst_ip="", dest=body.get("deploy_target", ""),
             proto="IEEE 802.1Qbv")
    return {"ok": True, "msg": "TAS schedule saved"}


def _delete_tas(con, body):
    con.execute("DELETE FROM tas_schedules WHERE id=?", (body.get("id"),))
    con.execute("DELETE FROM gcl_entries WHERE schedule_id=?", (body.get("id"),))
    con.commit()
    return {"ok": True, "msg": "TAS schedule deleted"}


HANDLERS = {
    "save_tas": _save_tas,
    "delete_tas": _delete_tas,
}
