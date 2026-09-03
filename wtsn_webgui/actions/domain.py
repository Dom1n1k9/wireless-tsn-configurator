"""TSN domain management actions."""


def _save_domain(con, body):
    if not body.get("id"):
        return {"ok": False, "msg": "domain id required"}
    con.execute("INSERT OR REPLACE INTO domains(id,name,description) VALUES(?,?,?)",
                (body["id"], body.get("name", body["id"]), body.get("description", "")))
    con.commit()
    return {"ok": True, "msg": "domain saved"}


def _delete_domain(con, body):
    did = body.get("id")
    if did == "default":
        return {"ok": False, "msg": "cannot delete default domain"}
    con.execute("DELETE FROM domains WHERE id=?", (did,))
    con.execute("UPDATE devices SET domain='default' WHERE domain=?", (did,))
    con.commit()
    return {"ok": True, "msg": "domain deleted"}


def _assign_domain(con, body):
    did = body.get("device_id")
    dom = body.get("domain")
    if not did or not dom:
        return {"ok": False, "msg": "device and domain required"}
    con.execute("UPDATE devices SET domain=? WHERE id=?", (dom, did))
    con.commit()
    return {"ok": True, "msg": "device assigned"}


HANDLERS = {
    "save_domain": _save_domain,
    "delete_domain": _delete_domain,
    "assign_domain": _assign_domain,
}
