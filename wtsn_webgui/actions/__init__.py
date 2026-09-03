"""Action handlers behind /api/action and /api/actions/<name> endpoints.

The per-domain modules (devices, qos, vlan, tas, timesync, streams, domain,
fxmqtt, misc) each expose `HANDLERS = {name: fn(con, body)}`. This package
`__init__` is the public dispatcher: it opens one DB connection, routes the
action, and always closes it. `run_action` is importable here for
backwards-compatibility with callers of the old single-file module.
"""
from . import devices, domain, fxmqtt, misc, qos, streams, tas, timesync, vlan
from ..db import connect
from ..mqtt_link import get_real_mqtt  # noqa: F401  (re-exported for callers)

_MODULES = (devices, domain, fxmqtt, misc, qos, streams, tas, timesync, vlan)

_REGISTRY = {}
for _mod in _MODULES:
    for _name, _fn in getattr(_mod, "HANDLERS", {}).items():
        _REGISTRY[_name] = _fn
del _mod, _name, _fn


def run_action(act, body):
    con = connect()
    handler = _REGISTRY.get(act)
    try:
        if handler is None:
            return {"ok": False, "msg": "unknown action: %s" % act}
        return handler(con, body)
    except Exception as ex:
        return {"ok": False, "msg": str(ex)}
    finally:
        con.close()
