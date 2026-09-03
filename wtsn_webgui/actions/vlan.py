"""VLAN (802.1Q WVLAN) group/membership actions."""
from ..db import add_event, clamp


def _save_vlan(con, body):
    vlan = clamp(body.get("vlan_id", 1), 1, 4094)
    gid = body.get("id") or ("grp%d" % vlan)
    con.execute("INSERT OR REPLACE INTO vlan_groups(id,name,vlan_id) VALUES(?,?,?)",
                (gid, body.get("name", ""), vlan))
    con.commit()
    add_event("vlan", "cnc", "VLAN group %s created (vlan_id %d, name %s)" %
              (gid, vlan, body.get("name", "")),
             src_ip="", dst_ip="", dest="", proto="IEEE 802.1Q (WVLAN)")
    return {"ok": True, "msg": "VLAN group saved"}


def _delete_vlan(con, body):
    con.execute("DELETE FROM vlan_groups WHERE id=?", (body.get("id"),))
    con.execute("DELETE FROM vlan_members WHERE group_id=?", (body.get("id"),))
    con.commit()
    return {"ok": True, "msg": "VLAN deleted"}


def _save_member(con, body):
    gid = body.get("group_id")
    grp = con.execute("SELECT vlan_id,name FROM vlan_groups WHERE id=?",
                     (gid,)).fetchone()
    gtag = grp["vlan_id"] if grp else "?"
    gname = grp["name"] if grp else gid
    if body.get("set_members") is not None:
        picked = [x for x in body.get("set_members") if x]
        con.execute("DELETE FROM vlan_members WHERE group_id=?", (gid,))
        for dev in picked:
            con.execute("INSERT OR REPLACE INTO vlan_members(group_id,device_id) VALUES(?,?)",
                        (gid, dev))
        con.commit()
        add_event("vlan", "cnc", "devices %s propagate VLAN tag %d (%s) via 802.1Q" %
                  (",".join(picked) if picked else "-", gtag, gname), src_ip="",
                 dst_ip="", dest=", ".join(picked), proto="IEEE 802.1Q (WVLAN)")
        return {"ok": True, "msg": "members updated"}
    dev = body.get("device_id", "")
    remove = bool(body.get("remove"))
    if remove:
        con.execute("DELETE FROM vlan_members WHERE group_id=? AND device_id=?",
                    (gid, dev))
        msg = "Member removed"
    elif dev:
        con.execute("INSERT OR REPLACE INTO vlan_members(group_id,device_id) VALUES(?,?)",
                    (gid, dev))
        msg = "Member added"
    else:
        con.execute("DELETE FROM vlan_members WHERE group_id=?", (gid,))
        msg = "All members removed from " + str(gid)
    con.commit()
    add_event("vlan", "cnc", "%s %s (VLAN tag %d, %s)" % (msg, dev, gtag, gname),
             src_ip="", dst_ip="", dest=dev or gid, proto="IEEE 802.1Q (WVLAN)")
    return {"ok": True, "msg": msg}


HANDLERS = {
    "save_vlan": _save_vlan,
    "delete_vlan": _delete_vlan,
    "save_member": _save_member,
}
