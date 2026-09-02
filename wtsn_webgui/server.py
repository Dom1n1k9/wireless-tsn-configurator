"""HTTP server: serves the static UI and the JSON API."""
import base64
import hashlib
import json
import os
import re
import secrets
import struct
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

from . import state
from .actions import run_action
from .db import get_events, load_all

WEB_HOST = os.environ.get("WTSN_HOST", "127.0.0.1")
WEB_USER = os.environ.get("WTSN_WEB_USER") or None
WEB_PASS = os.environ.get("WTSN_WEB_PASS") or ""
MAX_BODY = 1 << 20
MAX_FW = 8 << 20
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
FW_NAME_RE = re.compile(r"[A-Za-z0-9._-]+\.(bin|img|hex)")


class WSHub:
    """Minimal WebSocket fan-out: clients receive a JSON ping whenever state changes."""

    def __init__(self):
        self.socks = set()
        self.lock = threading.Lock()

    def add(self, s):
        with self.lock:
            self.socks.add(s)

    def remove(self, s):
        with self.lock:
            self.socks.discard(s)

    def broadcast(self, data):
        dead = []
        with self.lock:
            socks = list(self.socks)
        for s in socks:
            try:
                s.sendall(data)
            except Exception:
                dead.append(s)
        for s in dead:
            self.remove(s)


WS = WSHub()


def _frame(payload):
    if len(payload) < 126:
        return bytes([0x81, len(payload)]) + payload
    if len(payload) < 65536:
        return b"\x81\x7e" + struct.pack(">H", len(payload)) + payload
    return b"\x81\x7f" + struct.pack(">Q", len(payload)) + payload


def ws_broadcaster():
    """Push a refresh ping to every WebSocket client whenever state changes (capped 1/2s)."""
    while not state.LISTENER_STOP.is_set():
        state.WS_NOTIFY.wait(2.0)
        state.WS_NOTIFY.clear()
        if not WS.socks:
            continue
        payload = json.dumps({"t": "refresh", "mode": state.MODE["mode"]}).encode()
        WS.broadcast(_frame(payload))


def _basic_auth(user, pw):
    import base64
    return "Basic " + base64.b64encode(("%s:%s" % (user, pw)).encode("utf-8")).decode("ascii")


def _load_html():
    with open(os.path.join(STATIC_DIR, "index.html"), "rb") as f:
        return f.read()


def make_handler():
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, b, content_type="application/json"):
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def _check_auth(self):
            if WEB_USER is None:
                return True
            got = self.headers.get("Authorization", "")
            want = _basic_auth(WEB_USER, WEB_PASS)
            return secrets.compare_digest(got.encode("utf-8"), want.encode("utf-8"))

        def _read_json(self):
            n = int(self.headers.get("Content-Length", 0))
            if n < 0 or n > MAX_BODY:
                return None
            try:
                return json.loads(self.rfile.read(n) or b"{}")
            except Exception:
                return None

        def do_GET(self):
            if not self._check_auth():
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="WTSN Configurator"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            p = urlsplit(self.path).path
            if p == "/ws":
                self._handle_ws()
            elif p == "/api/data":
                d = load_all()
                d["mode"] = state.MODE["mode"]
                with state.EVENT_LOCK:
                    d["events"] = list(state.EVENTS)[:300]
                self._send(json.dumps(d).encode())
            elif p == "/api/events":
                self._send(json.dumps(get_events()).encode())
            elif p.startswith("/fw/"):
                self._serve_fw(p[len("/fw/"):])
            else:
                self._send(_load_html(), "text/html; charset=utf-8")

        def _recv_exact(self, n):
            buf = b""
            while len(buf) < n:
                chunk = self.rfile.read1(n - len(buf))
                if not chunk:
                    raise ConnectionError("closed")
                buf += chunk
            return buf

        def _handle_ws(self):
            key = self.headers.get("Sec-WebSocket-Key", "")
            if not key:
                self.send_error(400)
                return
            accept = base64.b64encode(
                hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
            self.send_response(101, "Switching Protocols")
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            sock = self.connection
            WS.add(sock)
            try:
                while not state.LISTENER_STOP.is_set():
                    hdr = self._recv_exact(2)
                    opcode = hdr[0] & 0x0F
                    ln = hdr[1] & 0x7F
                    if ln == 126:
                        ln = struct.unpack(">H", self._recv_exact(2))[0]
                    elif ln == 127:
                        ln = struct.unpack(">Q", self._recv_exact(8))[0]
                    mask = self._recv_exact(4) if hdr[1] & 0x80 else b""
                    data = self._recv_exact(ln) if ln else b""
                    if hdr[1] & 0x80:
                        data = bytes(c ^ mask[i % 4] for i, c in enumerate(data))
                    if opcode == 0x8:
                        break
                    if opcode == 0x9:
                        sock.sendall(_frame(b"\x8a"))
            except Exception:
                pass
            finally:
                WS.remove(sock)

        def _upload_fw(self):
            n = int(self.headers.get("Content-Length", 0))
            if n <= 0 or n > MAX_FW:
                self._send(json.dumps({"ok": False, "msg": "bad size (max %d)" % MAX_FW}).encode())
                return
            raw = self.rfile.read(n)
            name = os.path.basename(self.headers.get("X-Filename", ""))
            if not FW_NAME_RE.match(name):
                name = "firmware-%d.bin" % secrets.randbelow(10 ** 8)
            path = os.path.join(state.FW_DIR, name)
            with open(path, "wb") as f:
                f.write(raw)
            self._send(json.dumps({"ok": True, "file": name, "size": n,
                                    "url": "/fw/" + name}).encode())

        def _serve_fw(self, name):
            name = os.path.basename(name)
            if not FW_NAME_RE.match(name):
                self.send_error(404)
                return
            path = os.path.join(state.FW_DIR, name)
            if not os.path.isfile(path):
                self.send_error(404)
                return
            with open(path, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            if not self._check_auth():
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="WTSN Configurator"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            p = urlsplit(self.path).path
            if p == "/api/firmware":
                self._upload_fw()
                return
            if p == "/api/action":
                # legacy route: action name carried inside the JSON body
                body = self._read_json() or {}
                act = body.get("action")
                data = body.get("data", body)
            elif p.startswith("/api/actions/"):
                act = p[len("/api/actions/"):]
                data = self._read_json() or {}
            else:
                self.send_error(404)
                return
            res = run_action(act, data)
            self._send(json.dumps(res).encode())

    return H


class WTSNServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
