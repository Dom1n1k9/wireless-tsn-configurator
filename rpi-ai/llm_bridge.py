"""WTSN LLM Bridge (runs on the Raspberry Pi edge node).

HTTP service on 127.0.0.1:8081 that turns natural-language requests into
validated TSN actions. Flow:
  GUI chat -> webgui /api/actions/llm_chat -> this bridge -> Ollama (local
  LLM) -> strict JSON proposal -> allowlist validation -> GUI action API
  with source="llm" -> result back to the chat.

The LLM can ONLY propose actions from a fixed allowlist with clamped
parameters; anything else is refused. It never touches the network/MQTT
directly.

Env:
  WTSN_OLLAMA_MODEL   (default qwen2.5:1.5b)
  WTSN_OLLAMA_URL     (default http://127.0.0.1:11434)
  WTSN_WEB_USER / WTSN_WEB_PASS  (GUI basic auth, from /etc/wtsn/env)
"""
import base64
import json
import os
import re
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL = os.environ.get("WTSN_OLLAMA_MODEL", "qwen2.5:1.5b")
OLLAMA_URL = os.environ.get("WTSN_OLLAMA_URL", "http://127.0.0.1:11434")
GUI_URL = os.environ.get("WTSN_AI_GUI", "http://127.0.0.1:8000")
GUI_USER = os.environ.get("WTSN_WEB_USER", "")
GUI_PASS = os.environ.get("WTSN_WEB_PASS", "")

log_lock = threading.Lock()


def log(msg):
    with log_lock:
        print("[wtsn-llm] %s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def gui_headers():
    h = {"Accept": "application/json"}
    if GUI_USER:
        h["Authorization"] = "Basic " + base64.b64encode(
            ("%s:%s" % (GUI_USER, GUI_PASS)).encode()).decode()
    return h


def gui_json(path, body=None):
    data = None
    headers = gui_headers()
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(GUI_URL + path, data=data, headers=headers,
                                 method="POST" if body is not None else "GET")
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read().decode())


def gui_action(name, params, reason):
    body = dict(params)
    body["source"] = "llm"
    body["reason"] = reason
    return gui_json("/api/actions/" + name, body)


def list_devices():
    try:
        d = gui_json("/api/data")
        return [x["id"] for x in d.get("devices", [])]
    except Exception:  # noqa: BLE001
        return []


# ---------------- allowlist + validation ----------------
ALLOWED = {
    "save_qos": {"need": ["device_id"],
                 "clamp": {"priority": (0, 7), "traffic_class": (0, 3),
                           "bandwidth_kbps": (1, 1000000), "latency_ms": (0, 10000),
                           "preemption": (0, 2)}},
    "delete_qos": {"need": ["device_id"], "clamp": {}},
    "save_tas": {"need": ["cycle_time_ns"],
                 "clamp": {"cycle_time_ns": (100000, 10**9)}},
    "delete_tas": {"need": ["id"], "clamp": {}},
    "save_stream": {"need": ["talker"],
                    "clamp": {"vlan_id": (0, 4094), "priority": (0, 7),
                              "data_frame_prio": (0, 7),
                              "max_latency_ns": (1, 10**12),
                              "max_interval_ns": (1, 10**12)}},
    "delete_stream": {"need": ["stream_id"], "clamp": {}},
    "save_timesync": {"need": ["grandmaster"], "clamp": {"mode": (0, 1)}},
    "save_preemption": {"need": ["device_id"], "clamp": {"preemption": (0, 1)}},
    "create_version": {"need": [], "clamp": {}},
    "exec_all": {"need": [], "clamp": {}},
}
DEVICE_KEYS = ("device_id", "deploy_target", "talker", "grandmaster")


def validate(action, params, devices):
    spec = ALLOWED.get(action)
    if not spec:
        return None, "action not allowed: %s" % action
    if not isinstance(params, dict):
        params = {}
    for k in spec["need"]:
        if k not in params:
            return None, "missing required param: %s" % k
    cleaned = {}
    for k, v in params.items():
        if k in spec["clamp"]:
            lo, hi = spec["clamp"][k]
            try:
                v = int(v)
            except (TypeError, ValueError):
                return None, "param %s must be an integer" % k
            v = max(lo, min(hi, v))
        if k in DEVICE_KEYS:
            if v not in devices:
                return None, "unknown device: %s" % v
        if k == "listeners" and isinstance(v, list):
            v = [x for x in v if x in devices]
        cleaned[k] = v
    return cleaned, None


SYSTEM_PROMPT = """You are the TSN configuration assistant of the WTSN Configurator.
You decide IEEE TSN configuration actions from user requests in any language.
Known devices: {devices}

Reply with ONLY a strict JSON object, no markdown, no extra text:
{{"action": "<name>", "params": {{...}}, "reason": "<short reason>", "reply": "<short human answer>"}}

Allowed actions and their params:
- save_qos: device_id, priority(0-7), traffic_class(0-3), bandwidth_kbps, latency_ms, preemption(0-2)
- delete_qos: device_id
- save_tas: id, name, cycle_time_ns (min 100000), deploy_target, gcl:[{{"gate_state":int,"duration_ns":int}}]
- delete_tas: id
- save_stream: stream_id, name, talker, listeners:[ids], vlan_id, max_latency_ns, max_interval_ns, priority(0-7)
- delete_stream: stream_id
- save_timesync: mode(0-1), grandmaster, nodes:[ids]
- save_preemption: device_id, preemption(0-1), emac, pmac
- create_version: name
- exec_all: (empty) - deploys all saved config to the network

If the request is informational or you cannot map it to an allowed action, use:
{{"action": "none", "params": {{}}, "reason": "", "reply": "<answer>"}}
"""


def llm_chat(message, devices, history=None):
    prompt = SYSTEM_PROMPT.format(devices=", ".join(devices) or "(none)")
    msgs = [{"role": "system", "content": prompt}]
    for h in (history or [])[-6:]:
        r = str(h.get("role", "user"))[:10]
        if r in ("user", "assistant"):
            msgs.append({"role": r, "content": str(h.get("content", ""))[:2000]})
    msgs.append({"role": "user", "content": str(message)[:2000]})
    payload = json.dumps({"model": MODEL, "messages": msgs, "stream": False,
                          "options": {"temperature": 0.1, "num_predict": 400}}).encode()
    req = urllib.request.Request(OLLAMA_URL + "/api/chat", data=payload,
                                 headers={"Content-Type": "application/json"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=180) as r:
        out = json.loads(r.read().decode())
    log("llm answered in %.1fs" % (time.time() - t0))
    text = out.get("message", {}).get("content", "")
    m = re.search(r"\{.*\}", text, re.S)
    if not m:
        return {"ok": True, "reply": text.strip() or "(empty)", "action": "none"}
    return json.loads(m.group(0))


def handle_chat(body):
    message = str(body.get("message", ""))[:2000]
    if not message.strip():
        return {"ok": False, "msg": "empty message"}
    devices = list_devices()
    try:
        prop = llm_chat(message, devices, body.get("history"))
    except Exception as ex:  # noqa: BLE001
        log("llm error: %s" % ex)
        return {"ok": False, "msg": "LLM error: %s" % ex}
    action = str(prop.get("action", "none"))
    reply = str(prop.get("reply", ""))
    reason = str(prop.get("reason", "")) or ("LLM: " + message[:80])
    if action == "none":
        return {"ok": True, "reply": reply or "OK", "action": "none",
                "executed": []}
    params, err = validate(action, prop.get("params") or {}, devices)
    if err:
        return {"ok": False, "msg": "refused: " + err, "action": action,
                "reply": "Refused: " + err, "executed": []}
    executed = []
    res = gui_action(action, params, "LLM: " + reason)
    executed.append({"action": action, "params": params,
                     "ok": bool(res.get("ok")), "msg": res.get("msg", "")})
    if action not in ("exec_all", "create_version") and res.get("ok"):
        dep = gui_action("exec_all", {}, "LLM: deploy after " + action)
        executed.append({"action": "exec_all", "params": {},
                         "ok": bool(dep.get("ok")), "msg": dep.get("msg", "")})
    return {"ok": True, "reply": reply or ("done: " + action),
            "action": action, "reason": reason, "executed": executed}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path == "/health":
            self._send({"ok": True, "model": MODEL,
                        "url": OLLAMA_URL, "time": int(time.time())})
        else:
            self._send({"ok": False, "msg": "not found"}, 404)

    def do_POST(self):
        if self.path != "/chat":
            self._send({"ok": False, "msg": "not found"}, 404)
            return
        try:
            n = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(n) or b"{}")
        except Exception:  # noqa: BLE001
            self._send({"ok": False, "msg": "bad json"}, 400)
            return
        try:
            self._send(handle_chat(body))
        except Exception as ex:  # noqa: BLE001
            self._send({"ok": False, "msg": str(ex)}, 500)


def main():
    srv = ThreadingHTTPServer(("127.0.0.1", 8081), H)
    log("llm bridge on 127.0.0.1:8081 model=%s ollama=%s" % (MODEL, OLLAMA_URL))
    srv.serve_forever()


if __name__ == "__main__":
    main()
