"""Entry point: python -m wtsn_webgui (or the webgui.py shim at the repo root)."""
import os
import signal
import sys
import threading

from . import mqtt_link, state
from .mqtt_link import mqtt_listener_loop
from .server import WEB_HOST, WTSNServer, make_handler, ws_broadcaster
from .sim import sim_runner


def main(argv=None):
    host = WEB_HOST
    port = state.PORT
    args = list(sys.argv[1:] if argv is None else argv)
    i = 0
    while i < len(args):
        if args[i] in ("-h", "--help"):
            print("Usage: python3 webgui.py [--host H] [--port P] [--help]")
            print("  --host H      bind address (default %s, use 0.0.0.0 to expose)" % WEB_HOST)
            print("  --port P      port (default %d)" % port)
            print("Env: WTSN_HOST, WTSN_PORT, WTSN_DB, WTSN_BROKER, WTSN_USER,")
            print("     WTSN_PASS, WTSN_WEB_USER, WTSN_WEB_PASS")
            return 0
        elif args[i] == "--host" and i + 1 < len(args):
            host = args[i + 1]
            i += 2
        elif args[i] == "--port" and i + 1 < len(args):
            port = int(args[i + 1])
            i += 2
        else:
            i += 1

    state.LISTENER_STOP.clear()
    threading.Thread(target=sim_runner, daemon=True).start()
    threading.Thread(target=mqtt_listener_loop, daemon=True).start()
    threading.Thread(target=ws_broadcaster, daemon=True).start()

    srv = WTSNServer((host, port), make_handler())

    def _shutdown(sig, frame):
        state.LISTENER_STOP.set()
        try:
            srv.shutdown()
        except Exception:
            pass
        try:
            os._exit(0)
        except Exception:
            pass

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    scheme = "http"  # TLS is not bundled; run behind a reverse proxy for TLS.
    addr = "127.0.0.1" if host in ("127.0.0.1", "localhost") else host
    print("WTSN web GUI: %s://%s:%d  (db=%s)" % (scheme, addr, port, state.DB_REAL), flush=True)
    if host in ("127.0.0.1", "localhost"):
        try:
            import webbrowser
            threading.Thread(target=webbrowser.open,
                           args=("http://127.0.0.1:%d/" % port,),
                           daemon=True).start()
        except Exception:
            pass
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        state.LISTENER_STOP.set()
        with state.MQTT_LOCK:
            if mqtt_link.REAL_MQTT:
                try:
                    mqtt_link.REAL_MQTT.close()
                except Exception:
                    pass
                mqtt_link.REAL_MQTT = None
        srv.server_close()
        print("WTSN web GUI stopped", flush=True)
    return 0
