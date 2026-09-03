"""Tests for the wtsn_webgui package: action handlers and HTTP smoke tests."""
import json
import os
import shutil
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

from wtsn_webgui import state
from wtsn_webgui.actions import run_action
from wtsn_webgui.db import clamp, connect, load_all
from wtsn_webgui.mqtt_link import parse_listener_msg
from wtsn_webgui.server import WTSNServer, make_handler

class WebGuiActionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="wtsn_test_")
        cls._old_sim = state.DB_SIM
        cls._old_real = state.DB_REAL
        state.DB_SIM = os.path.join(cls.tmp, "sim.db")
        state.DB_REAL = os.path.join(cls.tmp, "real.db")
        state.MODE["mode"] = "sim"

    @classmethod
    def tearDownClass(cls):
        state.DB_SIM = cls._old_sim
        state.DB_REAL = cls._old_real
        state.MODE["mode"] = "sim"
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        state.EVENTS.clear()
        state.RECENT_ACKS.clear()

    def act(self, name, data=None):
        return run_action(name, data or {})

    def test_set_mode(self):
        r = self.act("set_mode", {"mode": "real"})
        self.assertTrue(r["ok"])
        self.assertEqual(state.MODE["mode"], "real")
        r = self.act("set_mode", {"mode": "bogus"})
        self.assertFalse(r["ok"])
        r = self.act("set_mode", {"mode": "sim"})
        self.assertTrue(r["ok"])
        self.assertEqual(state.MODE["mode"], "sim")

    def test_unknown_action(self):
        r = self.act("no_such_action")
        self.assertFalse(r["ok"])
        self.assertIn("unknown action", r["msg"])

    def test_clamp(self):
        self.assertEqual(clamp(9, 0, 7), 7)
        self.assertEqual(clamp("x", 0, 7), 0)
        self.assertEqual(clamp(None, 1, 5), 1)

    def test_domains(self):
        self.assertFalse(self.act("save_domain", {})["ok"])
        r = self.act("save_domain", {"id": "shopfloor", "name": "Shop Floor"})
        self.assertTrue(r["ok"])
        self.assertFalse(self.act("delete_domain", {"id": "default"})["ok"])
        self.assertTrue(self.act("delete_domain", {"id": "shopfloor"})["ok"])

    def test_devices_roundtrip(self):
        r = self.act("save_devices", {"device": {"id": "esp32-01", "name": "gw",
                                                 "ip": "192.168.1.10",
                                                 "tsn": ["802.1Q QoS", "gPTP 802.1AS"]}})
        self.assertTrue(r["ok"])
        data = load_all()
        ids = [d["id"] for d in data["devices"]]
        self.assertIn("esp32-01", ids)
        feats = [f["feature"] for f in data["device_tsn_features"]
                 if f["device_id"] == "esp32-01"]
        self.assertEqual(sorted(feats), ["802.1Q QoS", "gPTP 802.1AS"])
        self.assertTrue(self.act("save_devices", {"delete": ["esp32-01"]})["ok"])
        data = load_all()
        self.assertNotIn("esp32-01", [d["id"] for d in data["devices"]])

    def test_qos_clamped(self):
        self.act("save_devices", {"device": {"id": "d1"}})
        self.assertTrue(self.act("save_qos", {"device_id": "d1", "priority": 9})["ok"])
        data = load_all()
        q = [x for x in data["qos_configs"] if x["device_id"] == "d1"][0]
        self.assertEqual(q["priority"], 7)
        self.assertTrue(self.act("delete_qos", {"device_id": "d1"})["ok"])

    def test_vlan_and_members(self):
        self.act("save_devices", {"device": {"id": "d1"}})
        self.act("save_devices", {"device": {"id": "d2"}})
        self.assertTrue(self.act("save_vlan", {"id": "g1", "name": "Control",
                                               "vlan_id": 100})["ok"])
        self.assertTrue(self.act("save_member", {"group_id": "g1",
                                                 "set_members": ["d1", "d2"]})["ok"])
        data = load_all()
        self.assertEqual(len(data["vlan_members"]), 2)
        self.assertTrue(self.act("delete_vlan", {"id": "g1"})["ok"])
        self.assertEqual(len(load_all()["vlan_members"]), 0)

    def test_tas_gcl(self):
        self.assertTrue(self.act("save_tas", {
            "id": "s1", "name": "sched", "cycle_time_ns": 1000000,
            "deploy_target": "d1",
            "gcl": [{"gate_state": 1, "duration_ns": 300000},
                    {"gate_state": 3, "duration_ns": 700000}]})["ok"])
        data = load_all()
        self.assertEqual(len(data["gcl_entries"]), 2)
        self.assertTrue(self.act("delete_tas", {"id": "s1"})["ok"])

    def test_timesync(self):
        self.assertTrue(self.act("save_timesync",
                                 {"mode": 1, "grandmaster": "d1", "nodes": ["d2"]})["ok"])
        data = load_all()
        self.assertEqual(data["timesync_status"][0]["grandmaster"], "d1")

    def test_stream_lifecycle(self):
        self.act("save_devices", {"device": {"id": "talker1"}})
        self.act("save_devices", {"device": {"id": "l1"}})
        self.assertFalse(self.act("save_stream", {"name": "x"})["ok"])
        self.assertTrue(self.act("save_stream", {"stream_id": "st1", "name": "ctrl",
                                                 "talker": "talker1",
                                                 "listeners": ["l1"]})["ok"])
        self.assertTrue(self.act("deploy_stream", {"stream_id": "st1"})["ok"])
        data = load_all()
        self.assertEqual(data["tsn_streams"][0]["status"], 1)
        self.assertTrue(self.act("delete_stream", {"stream_id": "st1"})["ok"])

    def test_versions_snapshot_and_rollback(self):
        self.act("save_devices", {"device": {"id": "d1"}})
        self.act("save_qos", {"device_id": "d1", "priority": 5})
        r = self.act("create_version", {"name": "pre"})
        self.assertTrue(r["ok"])
        self.act("save_qos", {"device_id": "d1", "priority": 7})
        vers = self.act("list_versions")["versions"]
        self.assertTrue(vers)
        diff = self.act("diff_versions", {"a": vers[0]["id"]})
        self.assertTrue(diff["ok"])
        vid = vers[0]["id"]
        self.assertTrue(self.act("rollback_version", {"id": vid})["ok"])
        data = load_all()
        q = [x for x in data["qos_configs"] if x["device_id"] == "d1"][0]
        self.assertEqual(q["priority"], 5)

    def test_restore_backup(self):
        self.act("save_devices", {"device": {"id": "d9"}})
        data = load_all()
        backup = {t: data[t] for t in ("devices", "qos_configs", "settings")}
        backup["meta"] = {"mode": "sim"}
        self.act("save_devices", {"delete": ["d9"]})
        self.assertFalse(self.act("restore_backup", {})["ok"])
        self.assertTrue(self.act("restore_backup", {"data": backup})["ok"])
        self.assertIn("d9", [d["id"] for d in load_all()["devices"]])

    def test_sync_report(self):
        self.assertFalse(self.act("sync_report", {})["ok"])
        r = self.act("sync_report", {"device_id": "d1", "offset_ns": 100})
        self.assertTrue(r["ok"])

    def test_parse_listener_ack(self):
        con = connect()
        try:
            con.execute("INSERT OR REPLACE INTO devices(id,name) VALUES('d1','x')")
            con.commit()
            parse_listener_msg(con, "tsn/ack/d1", json.dumps({"id": "d1", "ok": True}))
            self.assertIn("d1", state.RECENT_ACKS)
        finally:
            con.close()

    def test_parse_listener_sensors(self):
        con = connect()
        try:
            payload = json.dumps({"id": "d2", "sensors": [
                {"sensor_id": "temp1", "type": 0, "value": 21.5, "unit": "C",
                 "healthy": 1}]})
            parse_listener_msg(con, "tsn/sensors/d2", payload)
            data = load_all()
            self.assertTrue(any(s["device_id"] == "d2" and s["sensor_id"] == "temp1"
                                for s in data["sensors"]))
        finally:
            con.close()


class MockBroker:
    """A stand-in for mqtt_broker.MqttBroker that records what was published."""

    def __init__(self):
        self.published = []

    def publish(self, topic, payload, qos=0):
        self.published.append((topic, payload))
        return True


class WebGuiRealModeTest(unittest.TestCase):
    """Integration: command/ack cycle over (a mocked) MQTT in REAL mode."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="wtsn_real_")
        cls._old_sim = state.DB_SIM
        cls._old_real = state.DB_REAL
        state.DB_SIM = os.path.join(cls.tmp, "sim.db")
        state.DB_REAL = os.path.join(cls.tmp, "real.db")

    @classmethod
    def tearDownClass(cls):
        state.DB_SIM = cls._old_sim
        state.DB_REAL = cls._old_real
        state.MODE["mode"] = "sim"
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        state.MODE["mode"] = "real"
        state.EVENTS.clear()
        state.RECENT_ACKS.clear()
        self.broker = MockBroker()
        from wtsn_webgui import mqtt_link
        self._orig = mqtt_link.get_real_mqtt
        mqtt_link.get_real_mqtt = lambda con: self.broker

    def tearDown(self):
        from wtsn_webgui import mqtt_link
        mqtt_link.get_real_mqtt = self._orig
        state.MODE["mode"] = "sim"

    def act(self, name, data=None):
        return run_action(name, data or {})

    def test_exec_all_publishes_apply_and_status(self):
        self.act("save_devices", {"device": {"id": "esp32-01", "name": "gw"}})
        r = self.act("exec_all")
        self.assertTrue(r["ok"])
        topics = [t for t, _ in self.broker.published]
        self.assertIn("tsn/cmd/esp32-01/apply", topics)
        self.assertIn("tsn/cmd/esp32-01/status", topics)

    def test_stream_deploy_uses_fx_cmd_topic(self):
        self.act("save_devices", {"device": {"id": "talker1"}})
        self.act("save_devices", {"device": {"id": "l1"}})
        self.act("save_stream", {"stream_id": "st1", "name": "ctrl",
                                 "talker": "talker1", "listeners": ["l1"]})
        r = self.act("deploy_stream", {"stream_id": "st1"})
        self.assertTrue(r["ok"])
        topics = [t for t, _ in self.broker.published]
        # FX command channel must be used, not the old tsn/fx/field/stream.
        self.assertIn("tsn/fx/cmd/talker1", topics)
        self.assertIn("tsn/cmd/talker1/stream", topics)
        self.assertIn("tsn/cmd/l1/stream", topics)
        self.assertNotIn("tsn/fx/field", topics)
        self.assertNotIn("tsn/fx/stream", topics)

    def test_ping_uses_device_command_topic(self):
        self.act("save_devices", {"device": {"id": "esp32-01"}})
        r = self.act("ping_device", {"id": "esp32-01"})
        self.assertTrue(r["ok"])
        self.assertIn(("tsn/cmd/esp32-01/ping", "1"), self.broker.published)

    def test_ack_recorded_in_state(self):
        con = connect()
        try:
            parse_listener_msg(con, "tsn/ack/esp32-01",
                               json.dumps({"id": "esp32-01", "ok": True}))
            self.assertIn("esp32-01", state.RECENT_ACKS)
            self.assertTrue(state.RECENT_ACKS["esp32-01"][0])
        finally:
            con.close()


class WebGuiHttpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="wtsn_http_")
        cls._old_sim = state.DB_SIM
        cls._old_real = state.DB_REAL
        state.DB_SIM = os.path.join(cls.tmp, "sim.db")
        state.DB_REAL = os.path.join(cls.tmp, "real.db")
        state.MODE["mode"] = "sim"
        cls.srv = WTSNServer(("127.0.0.1", 0), make_handler())
        cls.port = cls.srv.server_address[1]
        cls.thread = threading.Thread(target=cls.srv.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()
        state.DB_SIM = cls._old_sim
        state.DB_REAL = cls._old_real
        state.MODE["mode"] = "sim"
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def url(self, path):
        return "http://127.0.0.1:%d%s" % (self.port, path)

    def get(self, path):
        with urllib.request.urlopen(self.url(path), timeout=5) as r:
            return r.status, r.read()

    def post(self, path, payload):
        req = urllib.request.Request(
            self.url(path), data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, None

    def test_index_served(self):
        status, body = self.get("/")
        self.assertEqual(status, 200)
        self.assertIn(b"WTSN Configurator", body)

    def test_api_data(self):
        status, body = self.get("/api/data")
        self.assertEqual(status, 200)
        data = json.loads(body)
        self.assertEqual(data["mode"], "sim")
        self.assertIn("devices", data)

    def test_api_events(self):
        status, body = self.get("/api/events")
        self.assertEqual(status, 200)
        self.assertIn("events", json.loads(body))

    def test_action_new_route(self):
        status, res = self.post("/api/actions/save_domain",
                                {"id": "http-dom", "name": "Http Dom"})
        self.assertEqual(status, 200)
        self.assertTrue(res["ok"])

    def test_action_legacy_route(self):
        status, res = self.post("/api/action",
                                {"action": "save_domain",
                                 "data": {"id": "legacy-dom"}})
        self.assertEqual(status, 200)
        self.assertTrue(res["ok"])

    def test_unknown_action_json(self):
        status, res = self.post("/api/actions/nope", {})
        self.assertEqual(status, 200)
        self.assertFalse(res["ok"])

    def test_unknown_path_404(self):
        status, _ = self.post("/api/other", {})
        self.assertEqual(status, 404)


if __name__ == "__main__":
    unittest.main()
