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
    "save_vlan": {"need": ["vlan_id"], "clamp": {"vlan_id": (1, 4094)}},
    "delete_vlan": {"need": ["id"], "clamp": {}},
    "save_member": {"need": ["group_id"], "clamp": {}},
    "deploy_stream": {"need": ["stream_id"], "clamp": {}},
    "deploy_all_streams": {"need": [], "clamp": {}},
    "ping_device": {"need": ["device_id"], "clamp": {}},
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
        if k in ("listeners", "set_members") and isinstance(v, list):
            v = [x for x in v if x in devices]
        cleaned[k] = v
    return cleaned, None


SYSTEM_PROMPT = """You are the TSN configuration assistant of the WTSN Configurator (web GUI for a wireless TSN network: ESP32 nodes, cameras, sensors on an RPi edge).
You have TWO jobs, and you ALWAYS answer in English (even if the user writes in another language). Decide which job applies BEFORE choosing an action:

1. DO -- execute a change ONLY when the user imperatively orders a SPECIFIC change:
   an imperative verb (set, change, add, create, make, save, delete, remove,
   deploy, apply, ping, turn on, turn off) plus a concrete target (a device,
   value, stream, VLAN...). Then map it to ONE allowed action and execute it.
2. GUIDE -- answer with EXPLANATION ONLY and set action "none" (execute NOTHING)
   when the user is asking a question or how to do something. These ALWAYS mean
   GUIDE, never DO: "how do I ...", "how to ...", "what is ...", "what should I ...",
   "explain ...", "which page ...", "help", "from scratch", "walk me through",
   "a guide", "first steps". Explain step by step using the real GUI
   pages/buttons below. Keep guidance short and practical (max ~80 words),
   no marketing.

RULE: a question is never an order. Even if you know exactly which action would
help, if the user is ASKING (not imperatively ordering a concrete change), reply
with action "none" and only explain. When in doubt, choose GUIDE (none).

GUI map:
- Devices: list/add devices, status (online/offline/error), Ping, OTA firmware, camera clips
- IEEE 802.1Q -> QoS Priority: per-device priority 0-7 + latency budget (ms)
- IEEE 802.1Q -> WVLAN ID: VLAN groups (name, ID 1-4094), assign member devices
- IEEE 802.1Qbv -> TAS / GCL: name, cycle (ns), deploy target, gate list "state:ns,..." (state = bitmask of open queues)
- IEEE 802.1Qbu -> Preemption: on/off + express (eMAC) vs preemptable (pMAC) priority sets
- IEEE 802.1AS -> Synchronization: choose grandmaster + slave nodes
- IEEE 802.1Qcc -> TSN Streams: talker -> listeners on a VLAN, latency/interval budgets
- OPC UA FX over MQTT -> FXMQTT Config: MQTT broker host:port (the channel to devices)
- Monitor: live traffic/ACKs; Metrics: charts; Sensors: PIR/WiFi/sonar/AI detection + maps;
- Config Versions: snapshot + diff + rollback; AI Assistant (this chat): execute or guide;
- blue "Execute settings on controller" button in the header: deploys ALL saved config to the network

Recommended setup order for a new network: 1) FXMQTT broker, 2) add devices, 3) QoS priority, 4) VLAN, 5) TAS/GCL, 6) optional preemption / time sync / streams, 7) Execute settings on controller.

Reply with ONLY a strict JSON object, no markdown, no extra text:
{{"action": "<name>", "params": {{...}}, "reason": "<short reason>", "reply": "<answer in English>"}}
Keep "reply" SHORT so the answer streams back quickly:
- For an executed action: ONE short sentence confirming what was done (e.g.
  "Done: set esp32-02 QoS priority to 5 and deployed it."). Do NOT re-explain
  steps you already performed.
- For a guide: a compact step-by-step list, max ~80 words total.
The "reply" value may be several lines only for a guide — but keep it ONE JSON
string: write line breaks as \\n, never as a raw newline, and do not put
unescaped double-quotes inside it.

Allowed actions and their params:
- save_qos: device_id, priority(0-7), traffic_class(0-3), bandwidth_kbps, latency_ms, preemption(0-2)
- delete_qos: device_id
- save_tas: id, name, cycle_time_ns (min 100000), deploy_target, gcl:[{{"gate_state":int,"duration_ns":int}}]
- delete_tas: id
- save_stream: stream_id, name, talker, listeners:[ids], vlan_id, max_latency_ns, max_interval_ns, priority(0-7)
- delete_stream: stream_id
- save_timesync: mode(0-1), grandmaster, nodes:[ids]
- save_preemption: device_id, preemption(0-1), emac, pmac
- save_vlan: vlan_id(1-4094), id (group id, default grp<vlan_id>), name
- delete_vlan: id
- save_member: group_id, set_members:[device ids] (replaces the group's members)
- deploy_stream: stream_id
- deploy_all_streams: (empty)
- ping_device: device_id
- create_version: name
- exec_all: (empty) - deploys all saved config to the network

Known devices: {devices}

If the request is a question / guidance, or you cannot map it to an allowed
action, DO NOT execute anything -- use:
{{"action": "none", "params": {{}}, "reason": "", "reply": "<your answer in English>"}}
"""


def _repair_json(s):
    """Escape raw newlines/tabs/CR that appear INSIDE JSON string values —
    the most common small-model mistake when it writes a multi-line reply."""
    out = []
    in_str = False
    esc = False
    for ch in s:
        if in_str:
            if esc:
                out.append(ch)
                esc = False
            elif ch == "\\":
                out.append(ch)
                esc = True
            elif ch == '"':
                out.append(ch)
                in_str = False
            elif ch == "\n":
                out.append("\\n")
            elif ch == "\t":
                out.append("\\t")
            elif ch == "\r":
                out.append("\\r")
            else:
                out.append(ch)
        else:
            if ch == '"':
                in_str = True
            out.append(ch)
    return "".join(out)


def parse_llm_reply(text):
    """Turn the model's raw reply into {action, params, reason, reply}.

    The model writes its OWN answer; we only need to read it reliably. So:
      1. strict JSON parse,
      2. repaired JSON parse (unescaped newlines inside strings),
      3. best-effort extraction of the reply body,
      4. otherwise the model's full text as-is.
    It never raises and never discards the model's words.
    """
    prop = {"action": "none", "params": {}, "reason": "", "reply": (text or "").strip()}
    if not text or not text.strip():
        return prop
    t = text.strip()
    if t.startswith("```"):
        t = re.sub(r"^```[a-zA-Z]*\s*", "", t)
        t = re.sub(r"\s*```$", "", t).strip()
    start, end = t.find("{"), t.rfind("}")
    if start >= 0 and end > start:
        for attempt in (t[start:end + 1], _repair_json(t[start:end + 1])):
            try:
                obj = json.loads(attempt)
            except Exception:  # noqa: BLE001
                continue
            if not isinstance(obj, dict):
                continue
            for k in ("action", "reason", "reply"):
                if isinstance(obj.get(k), str) and obj[k].strip():
                    prop[k] = obj[k].strip()
            if isinstance(obj.get("params"), dict):
                prop["params"] = obj["params"]
            return prop
    m = re.search(r'"reply"\s*:\s*"', t)
    if m:
        body = t[m.end():].strip()
        if body.endswith("}"):
            body = body[:-1].strip()
        if body.endswith('"'):
            body = body[:-1].strip()
        if body:
            prop["reply"] = body
    prop["reply"] = prop["reply"].replace("\\n", "\n").replace('\\"', '"').replace("\\t", "\t")
    return prop


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
    prop = parse_llm_reply(text)
    if not prop["reply"]:
        prop["reply"] = "(empty)"
    return prop


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
