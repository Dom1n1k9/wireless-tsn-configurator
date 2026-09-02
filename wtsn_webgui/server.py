"""HTTP server: serves the static UI and the JSON API."""
import json
import os
import secrets
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

from . import state
from .actions import run_action
from .db import get_events, load_all

WEB_HOST = os.environ.get("WTSN_HOST", "127.0.0.1")
WEB_USER = os.environ.get("WTSN_WEB_USER") or None
WEB_PASS = os.environ.get("WTSN_WEB_PASS") or ""
MAX_BODY = 1 << 20
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")


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
            if p == "/api/data":
                d = load_all()
                d["mode"] = state.MODE["mode"]
                with state.EVENT_LOCK:
                    d["events"] = list(state.EVENTS)[:300]
                self._send(json.dumps(d).encode())
            elif p == "/api/events":
                self._send(json.dumps(get_events()).encode())
            else:
                self._send(_load_html(), "text/html; charset=utf-8")

        def do_POST(self):
            if not self._check_auth():
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="WTSN Configurator"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            p = urlsplit(self.path).path
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
