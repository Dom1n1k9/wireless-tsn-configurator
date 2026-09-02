"""MQTT 3.1.1 client wrapper (backed by paho-mqtt) with a small,
synchronous, thread-safe surface used by the rest of the app.

Methods: connect/subscribe/publish/close/recv_publish.
recv_publish blocks for the next inbound publication and returns (topic,payload),
or None when the connection is lost.
"""
import os
import threading
import time
from collections import deque

import paho.mqtt.client as mqttlib


class MqttBroker:
    def __init__(self, host="127.0.0.1", port=1883, client_id="wtsn-webgui"):
        self.host, self.port, self.client_id = host, port, client_id
        self.username = os.environ.get("WTSN_USER", "")
        self.password = os.environ.get("WTSN_PASS", "")
        self._paho = None
        self._inbox = deque()
        self._inbox_cv = threading.Condition()
        self._connected = False
        self._lock = threading.Lock()

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        self._connected = (reason_code == 0) if hasattr(reason_code, "is_failure") else (reason_code == 0)

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
            c.on_connect = self._on_connect
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
            def closed():
                return not self._paho or (not self._connected and not self._inbox)

            if timeout is None:
                while not self._inbox:
                    if closed():
                        return None
                    if not self._inbox_cv.wait(timeout=1.0):
                        continue
            else:
                deadline = time.monotonic() + timeout
                while not self._inbox:
                    if closed():
                        return None
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        return None
                    self._inbox_cv.wait(timeout=min(1.0, remaining))
            if self._inbox:
                return self._inbox.popleft()
            return None

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
