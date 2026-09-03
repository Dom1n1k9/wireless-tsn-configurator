"""MQTT 3.1.1 client wrapper (backed by paho-mqtt) with a small,
synchronous, thread-safe surface used by the rest of the app.

Methods: connect/subscribe/publish/close/recv_publish.
recv_publish blocks for the next inbound publication and returns (topic,payload),
or None when the connection is lost.
"""
import logging
import os
import threading
import time
from collections import deque

import paho.mqtt.client as mqttlib

log = logging.getLogger("wtsn.mqtt_broker")

# Sentinel returned by recv_publish when the timeout (not a disconnect) expired,
# so callers can keep waiting without treating it as a lost connection.
NONE_PUB = ("", None)


class MqttBroker:
    def __init__(self, host="127.0.0.1", port=1883, client_id="wtsn-webgui"):
        self.host, self.port, self.client_id = host, port, client_id
        self.username = os.environ.get("WTSN_USER", "")
        self.password = os.environ.get("WTSN_PASS", "")
        self.tls_ca = os.environ.get("WTSN_TLS_CA", "")
        self.tls_cert = os.environ.get("WTSN_TLS_CERT", "")
        self.tls_key = os.environ.get("WTSN_TLS_KEY", "")
        self.tls_insecure = os.environ.get("WTSN_TLS_INSECURE", "") in ("1", "true", "yes")
        self._paho = None
        self._inbox = deque()
        self._inbox_cv = threading.Condition()
        self._connected = False
        self._lock = threading.Lock()

    def _apply_tls(self, c):
        """Configure TLS when WTSN_TLS_CA (a CA bundle) is provided. Optional
        client cert/key then authenticate this client to the broker."""
        if not self.tls_ca:
            return
        try:
            c.tls_set(ca_certs=self.tls_ca,
                      certfile=self.tls_cert or None,
                      keyfile=self.tls_key or None)
            if self.tls_insecure:
                c.tls_insecure_set(True)
        except Exception:
            log.exception("MQTT TLS setup failed (WTSN_TLS_*)")

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        # paho v2: reason_code may carry .is_failure; v1 passes an int.
        ok = getattr(reason_code, "is_failure", None)
        self._connected = False if ok is None else not ok

    def _on_disconnect(self, client, userdata, flags, reason_code=None, properties=None):
        self._connected = False

    def connect(self):
        try:
            existing = self._paho
            self._paho = None
            self._connected = False
            if existing:
                try:
                    existing.loop_stop()
                except Exception:
                    pass
                try:
                    existing.disconnect()
                except Exception:
                    pass
            c = mqttlib.Client(mqttlib.CallbackAPIVersion.VERSION2,
                                client_id=self.client_id)
            if self.username:
                c.username_pw_set(self.username, self.password)
            self._apply_tls(c)
            c.on_connect = self._on_connect
            c.on_disconnect = self._on_disconnect
            c.on_message = self._on_message
            c.reconnect_delay_set(min_delay=1, max_delay=5)
            c.connect(self.host, self.port, keepalive=60)
            c.loop_start()
            self._paho = c
            self._connected = True
            return True
        except Exception:
            self._paho = None
            self._connected = False
            return False

    def _on_message(self, client, userdata, msg):
        with self._inbox_cv:
            self._inbox.append((msg.topic, msg.payload.decode("utf-8", "replace")))
            self._inbox_cv.notify()

    def subscribe(self, topic, qos=0):
        if not self._paho or not self._connected:
            return False
        try:
            self._paho.subscribe(topic, qos)
            return True
        except Exception:
            return False

    def publish(self, topic, payload, qos=0):
        if not self._paho or not self._connected:
            return False
        try:
            self._paho.publish(topic, payload, qos)
            return True
        except Exception:
            return False

    def recv_publish(self, timeout=None):
        """Block until an inbound publication arrives, then return (topic,payload).
        Returns None when the underlying connection is gone/closing."""
        with self._inbox_cv:
            def disconnected():
                return not self._paho or (not self._connected and not self._inbox)

            if timeout is None:
                while True:
                    if self._inbox:
                        return self._inbox.popleft()
                    if disconnected():
                        return None
                    self._inbox_cv.wait(timeout=1.0)
            else:
                deadline = time.monotonic() + timeout
                while True:
                    if self._inbox:
                        return self._inbox.popleft()
                    if disconnected():
                        return None
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        return NONE_PUB
                    self._inbox_cv.wait(timeout=min(1.0, remaining))
    def close(self):
        with self._lock:
            if self._paho:
                try:
                    self._paho.loop_stop()
                    self._paho.disconnect()
                except Exception:
                    pass
                self._paho = None
            self._connected = False
            with self._inbox_cv:
                self._inbox.clear()
                self._inbox_cv.notify_all()
