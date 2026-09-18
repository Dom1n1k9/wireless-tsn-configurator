"""Action handlers behind /api/action and /api/actions/<name> endpoints.

The per-domain modules (devices, qos, vlan, tas, timesync, streams, domain,
fxmqtt, misc) each expose `HANDLERS = {name: fn(con, body)}`. This package
`__init__` is the public dispatcher: it opens one DB connection, routes the
action, and always closes it. `run_action` is importable here for
backwards-compatibility with callers of the old single-file module.
"""
from . import (devices, domain, fxmqtt, misc, qos, streams, tas, timesync,
               topology, vlan)
from ..db import connect
from ..mqtt_link import get_real_mqtt  # noqa: F401  (re-exported for callers)

_MODULES = (devices, domain, fxmqtt, misc, qos, streams, tas, timesync,
            topology, vlan)

_REGISTRY = {}
for _mod in _MODULES:
    for _name, _fn in getattr(_mod, "HANDLERS", {}).items():
        _REGISTRY[_name] = _fn
del _mod, _name, _fn

import json as _json
import time as _time


def _record_decision(con, act, body, ok, msg):
    """Provenance: remember who changed what and why (user / AI / LLM)."""
    source = str(body.get("source", "") or "")
    reason = str(body.get("reason", "") or "")
    if not source and not reason:
        return
    if not source:
        source = "user"
    device_id = str(body.get("device_id", "") or body.get("id", "") or
                    body.get("grandmaster", "") or body.get("talker", "") or "")
    safe = {k: v for k, v in body.items() if k not in ("source", "reason", "pass")}
    con.execute("INSERT INTO ai_decisions(ts,device_id,action,params,reason,"
                "source,status) VALUES(?,?,?,?,?,?,?)",
                (int(_time.time()), device_id, act,
                 _json.dumps(safe)[:2000], reason[:500], source,
                 "ok" if ok else ("error: " + msg)[:200]))
    con.commit()


def run_action(act, body):
    con = connect()
    handler = _REGISTRY.get(act)
    if body is None:
        body = {}
    if not isinstance(body, dict):
        body = {}
    try:
        if handler is None:
            return {"ok": False, "msg": "unknown action: %s" % act}
        res = handler(con, body)
        try:
            _record_decision(con, act, body, bool(res.get("ok")),
                             str(res.get("msg", "")))
        except Exception:  # noqa: BLE001 - provenance must never break the action
            pass
        return res
    except Exception as ex:
        try:
            _record_decision(con, act, body, False, str(ex))
        except Exception:  # noqa: BLE001
            pass
        return {"ok": False, "msg": str(ex)}
    finally:
        con.close()
