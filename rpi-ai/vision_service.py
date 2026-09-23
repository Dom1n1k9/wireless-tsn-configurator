"""WTSN AI Vision service (runs on the Raspberry Pi edge node).

Pulls MJPEG streams from the ESP32-CAM nodes, runs YOLOv4-tiny object
detection (person + 80 COCO classes) and, when a target is detected:
  * triggers the ESP32-CAM to record its own microSD clip by publishing a
    motion event on  tsn/sensors/event  (the firmware already listens to it),
  * publishes live detection counters as sensors on  tsn/sensors/<id>
    (ai_detect / ai_person) so the web GUI Sensors page shows them,
  * keeps a rolling "last detection" thumbnail + short MJPEG clip locally.

Config file (JSON), default /home/wtsn/wtsn-ai/config.json:
  {
    "broker": "localhost:1883",
    "interval_s": 5,
    "conf": 0.45,
    "detect": ["person"],          // classes that trigger recording; "all" = any
    "cameras": [
      {"id": "esp32-cam", "url": "http://192.168.0.50/stream"}
    ]
  }
Override with env WTSN_AI_CONFIG.
"""
import json
import os
import re
import socket
import sys
import threading
import time

try:
    import cv2
    import numpy as np
except ImportError:
    print("ERROR: opencv-python and numpy required "
          "(sudo apt install python3-opencv python3-numpy)", file=sys.stderr)
    sys.exit(1)

import paho.mqtt.client as paho

MODEL_DIR = os.path.join(os.path.expanduser("~"), "wtsn-ai", "models")
CLIP_DIR = os.path.join(os.path.expanduser("~"), "wtsn-ai", "clips")
YOLO_CFG = "https://raw.githubusercontent.com/AlexeyAB/darknet/master/cfg/yolov4-tiny.cfg"
YOLO_W = "https://github.com/AlexeyAB/darknet/releases/download/yolov4/yolov4-tiny.weights"
COCO_NAMES = "https://raw.githubusercontent.com/pjreddie/darknet/master/data/coco.names"

log_lock = threading.Lock()


def log(msg):
    with log_lock:
        print("[wtsn-ai] %s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def ensure_models():
    os.makedirs(MODEL_DIR, exist_ok=True)
    cfg = os.path.join(MODEL_DIR, "yolov4-tiny.cfg")
    wt = os.path.join(MODEL_DIR, "yolov4-tiny.weights")
    names = os.path.join(MODEL_DIR, "coco.names")
    import urllib.request

    def fetch(url, path):
        if os.path.isfile(path) and os.path.getsize(path) > 0:
            return
        log("downloading %s" % os.path.basename(path))
        try:
            urllib.request.urlretrieve(url, path + ".tmp")
            os.replace(path + ".tmp", path)
        except Exception as ex:  # noqa: BLE001
            log("download failed %s: %s" % (url, ex))
    fetch(YOLO_CFG, cfg)
    fetch(YOLO_W, wt)
    fetch(COCO_NAMES, names)
    if not (os.path.isfile(cfg) and os.path.isfile(wt) and os.path.isfile(names)):
        log("model files incomplete - detection disabled until they are present")
        return None
    return cfg, wt, names


class Yolo:
    """Tiny YOLOv4-tiny inference wrapper on the OpenCV DNN backend."""

    def __init__(self, cfg, weights, names):
        self.net = cv2.dnn.readNetFromDarknet(cfg, weights)
        self.net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
        self.net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)
        self.names = [l.strip() for l in open(names).read().splitlines() if l.strip()]
        self.anchors = [[10, 16, 32, 32, 32, 42], [64, 73, 93, 99, 173, 168]]
        self.nh, self.nw = 416, 416
        self.out_names = ["2520", "2526"]

    def detect(self, frame):
        h, w = frame.shape[:2]
        blob = cv2.dnn.blobFromImage(frame, 1 / 255.0, (self.nw, self.nh),
                                     swapRB=True, crop=False)
        self.net.setInput(blob)
        out = self.net.forward(self.out_names)
        dets = []
        for gi, grid in enumerate([25, 13]):
            for oi, out_i in enumerate(out):
                data = out_i.reshape(3 * (len(self.names) + 5), grid * grid)
                data = data.T
                for a in range(3):
                    row = data[:, a]
                    for cell in range(row.shape[0]):
                        bx = row[cell]
                        if bx[4] < 0.1:
                            continue
                        cls_id = int(bx[5:].argmax())
                        conf = float(bx[4] * bx[5 + cls_id])
                        if conf < 0.1:
                            continue
                        cx = (bx[0] * (cell % grid) + 0.5) / grid * w
                        cy = (bx[1] * (cell // grid) + 0.5) / grid * h
                        bw = (bx[2] * bx[3])
                        bw = bw if bw > 0 else 1
                        # decode box dims via anchors (w/h scaled to input then to frame)
                        aw, ah = self.anchors[gi][2 * a], self.anchors[gi][2 * a + 1]
                        bw = (2 * bx[2]) ** 2 * aw * (w / self.nw)
                        bh = (2 * bx[3]) ** 2 * ah * (h / self.nh)
                        x1 = max(0, int(cx - bw / 2))
                        y1 = max(0, int(cy - bh / 2))
                        x2 = min(w, int(cx + bw / 2))
                        y2 = min(h, int(cy + bh / 2))
                        dets.append((conf, cls_id, x1, y1, x2, y2))
        dets.sort(key=lambda d: d[0], reverse=True)
        keep = []
        for d in dets:
            if all(cv2.intersectRect((d[2], d[3], d[4] - d[2], d[5] - d[3]),
                                      (k[2], k[3], k[4] - k[2], k[5] - k[3]))[2] *
                   cv2.intersectRect((d[2], d[3], d[4] - d[2], d[5] - d[3]),
                                      (k[2], k[3], k[4] - k[2], k[5] - k[3]))[3]
                   < 0.3 * min((d[4] - d[2]) * (d[5] - d[3]), (k[4] - k[2]) * (k[5] - k[3]))
                       for k in keep):
                keep.append(d)
            if len(keep) >= 12:
                break
        return [(self.names[c], float(conf), x1, y1, x2, y2)
                for conf, c, x1, y1, x2, y2 in keep]


def mjpeg_frames(url, timeout=8):
    """Yield decoded BGR frames from a multipart MJPEG HTTP stream."""
    import urllib.request

    req = urllib.request.Request(url, headers={"User-Agent": "wtsn-ai/1.0"})
    resp = urllib.request.urlopen(req, timeout=timeout)
    ctype = resp.headers.get("Content-Type", "")
    boundary = b"--123456789000000000000987654321"
    for part in ctype.split(b"="):
        if b"boundary" in part.lower() or b"--" in part:
            boundary = part.split(b"=")[-1].strip()
            break
    sock_timeout = socket.getdefaulttimeout()
    socket.setdefaulttimeout(timeout)
    try:
        buf = b""
        while True:
            chunk = resp.read(1 << 16)
            if not chunk:
                raise ConnectionError("stream closed")
            buf += chunk
            while True:
                start = buf.find(boundary)
                if start < 0:
                    break
                if len(buf) > 8 * (1 << 20):
                    buf = buf[-(1 << 16):]
                    break
                seg = buf[start:]
                end = seg.find(boundary, 2)
                if end < 0:
                    break
                part = seg[:end + len(boundary)]
                buf = buf[start + len(boundary):]
                if b"image/jpeg" not in part:
                    continue
                jpg = part.split(b"\r\n\r\n", 1)
                if len(jpg) < 2:
                    continue
                arr = np.frombuffer(jpg[1], np.uint8)
                frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
                if frame is not None:
                    yield frame
    finally:
        socket.setdefaulttimeout(sock_timeout)
        resp.close()


def synthetic_frames():
    """Infinite BGR frames with a moving 'person' blob — a demo camera source
    so the full detect -> record -> serve -> play pipeline can be exercised
    without physical hardware. Yields (frame, box) tuples."""
    W, H = 640, 480
    x = 0
    while True:
        img = np.zeros((H, W, 3), np.uint8)
        img[:] = (42, 38, 32)
        cv2.line(img, (0, H - 80), (W, H - 80), (80, 78, 70), 2)
        x = (x + 7) % (W + 140) - 70
        bw, bh = 72, 150
        y2 = H - 80
        y1 = y2 - bh
        x1 = x
        cv2.circle(img, (x1 + bw // 2, y1 - 20), 22, (0, 190, 255), -1)
        cv2.rectangle(img, (x1, y1), (x1 + bw, y2), (0, 190, 255), -1)
        cv2.putText(img, time.strftime("%H:%M:%S"), (10, 26),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (210, 210, 210), 1)
        cv2.putText(img, "SYNTHETIC", (W - 110, 26),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (120, 120, 120), 1)
        yield img, (max(0, x1), max(0, y1 - 42), min(W, x1 + bw), y2)
        time.sleep(0.05)


class Camera:
    def __init__(self, cfg):
        self.id = cfg["id"]
        self.url = cfg["url"]
        self.last_dets = []
        self.last_person = 0
        self.last_ok = time.time()
        self.relay = threading.Event()
        # Extract the numeric IP the camera is actually reachable at (from the
        # working stream URL). Announced to the GUI so it can correct a stale
        # DHCP-assigned address from an old discovery message.
        self.ip = ""
        m = re.search(r"//([0-9]{1,3}(?:\.[0-9]{1,3}){3})(?::\d+)?", self.url or "")
        if m:
            self.ip = m.group(1)

    def publish(self, cli, conf):
        payload = {"id": self.id, "ip": self.ip,
                   "sensors": [
                       {"sensor_id": "ai_detect", "value": len(self.last_dets),
                        "unit": "", "ts": int(time.time())},
                       {"sensor_id": "ai_person",
                        "value": 1 if self.last_person else 0,
                        "unit": "", "ts": int(time.time())},
                   ]}
        cli.publish("tsn/sensors/%s" % self.id, json.dumps(payload))
        brief = ", ".join("%s:%.2f" % (n, c) for n, c, *_ in self.last_dets[:4])
        cli.publish("tsn/sensors/event", json.dumps(
            {"id": self.id, "ip": self.ip, "ai": 1 if self.last_dets else 0,
             "motion": 1 if self.triggered else 0, "classes": brief}))
        if self.triggered:
            log("%s: MOTION TRIGGER -> %s" % (self.id, brief))

    def _frame_and_dets(self, yolo, conf):
        """Read one frame (real MJPEG stream or synthetic) and detect objects."""
        if self.url == "synthetic":
            if getattr(self, "_syn", None) is None:
                self._syn = synthetic_frames()
            frame, box = next(self._syn)
            dets = [("person", 0.9, *box)]
            return frame, dets
        frame = next(mjpeg_frames(self.url))
        try:
            dets = yolo.detect(frame)
        except Exception as ex:  # noqa: BLE001
            log("%s: detect error: %s" % (self.id, ex))
            dets = []
        return frame, dets

    def record_clip(self, yolo, conf):
        """Record a short annotated MJPEG clip and store it for the GUI."""
        cdir = os.path.join(CLIP_DIR, self.id)
        os.makedirs(cdir, exist_ok=True)
        ts = int(time.time())
        path = os.path.join(cdir, "clip_%d.mjpeg" % ts)
        n = int(conf.get("clip_frames", 20))
        delay = float(conf.get("clip_delay", 0.05))
        boundary = b"----wtsnclip"
        frames = []
        last_draw = None
        for _ in range(n):
            try:
                frame, dets = self._frame_and_dets(yolo, conf)
            except Exception:  # noqa: BLE001
                break
            draw = frame.copy()
            want = conf.get("detect", ["person"])
            for nm, c, x1, y1, x2, y2 in dets:
                col = (0, 0, 255) if nm in want else (255, 200, 0)
                cv2.rectangle(draw, (x1, y1), (x2, y2), col, 2)
                cv2.putText(draw, "%s %.2f" % (nm, c), (x1, max(12, y1 - 4)),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, col, 1)
            last_draw = draw
            ok, jpg = cv2.imencode(".jpg", draw, [cv2.IMWRITE_JPEG_QUALITY, 75])
            if ok:
                frames.append(jpg.tobytes())
            time.sleep(delay)
        if not frames or last_draw is None:
            return None
        try:
            with open(path, "wb") as f:
                for fb in frames:
                    f.write(b"\r\n" + boundary + b"\r\nContent-Type: image/jpeg\r\n"
                            b"Content-Length: " + str(len(fb)).encode() + b"\r\n\r\n" + fb)
                f.write(b"\r\n" + boundary + b"--\r\n")
            cv2.imwrite(os.path.join(cdir, "last.jpg"), last_draw,
                        [cv2.IMWRITE_JPEG_QUALITY, 80])
            # Keep a stable "latest" clip at last.mjpeg (same tricks as
            # last.jpg) so the web GUI can always replay the newest recording
            # even if the numbered clip_*.mjpeg files get pruned.
            with open(os.path.join(cdir, "last.mjpeg"), "wb") as f:
                for fb in frames:
                    f.write(b"\r\n" + boundary + b"\r\nContent-Type: image/jpeg\r\n"
                            b"Content-Length: " + str(len(fb)).encode() + b"\r\n\r\n" + fb)
                f.write(b"\r\n" + boundary + b"--\r\n")
            # keep the most recent 10 clips (last.mjpeg is the stable "latest"
            # so it is never pruned; it is advertised separately below)
            clips = sorted(f for f in os.listdir(cdir)
                           if f.endswith(".mjpeg") and f != "last.mjpeg")
            for old in clips[:-10]:
                try:
                    os.remove(os.path.join(cdir, old))
                except OSError:
                    pass
            keep = [c for c in clips if os.path.exists(os.path.join(cdir, c))][-10:]
            if os.path.exists(os.path.join(cdir, "last.mjpeg")):
                keep.append("last.mjpeg")
            mqtt_cli.publish("tsn/cam/recordings", json.dumps(
                {"id": self.id, "recordings": ["/ai/" + c for c in keep]}))
            log("%s: recorded clip %s (%d frames)" % (self.id, os.path.basename(path), len(frames)))
            return path
        except Exception as ex:  # noqa: BLE001
            log("%s: clip save error: %s" % (self.id, ex))
            return None

    def work(self, yolo, conf):
        while not self.relay.is_set():
            self.triggered = False
            try:
                frame, dets = self._frame_and_dets(yolo, conf)
            except Exception as ex:  # noqa: BLE001
                log("%s: stream unavailable (%s) - retrying" % (self.id, ex.__class__.__name__))
                time.sleep(conf.get("interval_s", 5))
                continue
            self.last_dets = dets
            self.last_person = 1 if any(n == "person" for n, *_ in dets) else 0
            want = conf.get("detect", ["person"])
            target = (want == ["all"] and dets) or \
                     any(n in want for n, *_ in dets)
            self.triggered = bool(target)
            if target:
                self.record_clip(yolo, conf)
            self.last_ok = time.time()
            self.publish(mqtt_cli, conf)
            time.sleep(conf.get("interval_s", 5))


def load_config():
    path = os.environ.get("WTSN_AI_CONFIG",
                          os.path.join(os.path.expanduser("~"), "wtsn-ai", "config.json"))
    conf = {"broker": "localhost:1883", "interval_s": 5, "conf": 0.45,
            "detect": ["person"], "cameras": []}
    if os.path.isfile(path):
        try:
            conf.update(json.load(open(path)))
        except Exception as ex:  # noqa: BLE001
            log("bad config %s: %s" % (path, ex))
    return conf


mqtt_cli = None


def main():
    global mqtt_cli
    conf = load_config()
    if not conf.get("cameras"):
        log("no cameras configured - add entries to "
            "/home/wtsn/wtsn-ai/config.json (cameras: [{id, url}])")
    os.makedirs(CLIP_DIR, exist_ok=True)
    broker = conf.get("broker", "localhost:1883")
    host, _, port = broker.partition(":")
    cli = paho.Client(paho.CallbackAPIVersion.VERSION2, "wtsn-ai-%d" % os.getpid())
    mqtt_cli = cli
    u = os.environ.get("WTSN_USER", "")
    p = os.environ.get("WTSN_PASS", "")
    if u:
        cli.username_pw_set(u, p)
    cli.connect(host, int(port or 1883), 60)
    cli.loop_start()
    log("connected to broker %s" % broker)

    model = ensure_models()
    yolo = Yolo(*model) if model else None
    if yolo is None:
        log("running WITHOUT AI model (motion trigger disabled until model downloads)")

    threads = []
    for cc in conf.get("cameras", []):
        cam = Camera(cc)
        if yolo or cc.get("url") == "synthetic":
            t = threading.Thread(target=cam.work, args=(yolo, conf), daemon=True)
        else:
            def idle(c=cam, cf=conf):
                while not c.relay.is_set():
                    log("%s: waiting for model" % c.id)
                    time.sleep(30)
            t = threading.Thread(target=idle, daemon=True)
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    if not threads:
        log("idle - waiting (add cameras to config.json, no restart needed "
            "within 60 s)")
        while True:
            time.sleep(1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
