"""Tests for the WTSN LLM bridge (rpi-ai/llm_bridge.py).

Only the deterministic parts are tested here: JSON proposal parsing, allowlist
validation, and the chat dispatch (guide/none never executes, a valid action
executes + auto-deploys, invalid proposals are refused). The Ollama call itself
is mocked out, so no model is required.
"""
import importlib.util
import os
import unittest


def _load_bridge():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "rpi-ai", "llm_bridge.py")
    spec = importlib.util.spec_from_file_location("wtsn_llm_bridge", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class LlmBridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bridge = _load_bridge()

    def setUp(self):
        self._saved = {}
        for name in ("llm_chat", "gui_action", "list_devices"):
            self._saved[name] = getattr(self.bridge, name)

    def tearDown(self):
        for name, fn in self._saved.items():
            setattr(self.bridge, name, fn)

    def _mock_chat(self, prop):
        self.bridge.llm_chat = lambda message, devices, history=None: dict(prop)
        self.bridge.list_devices = lambda: ["esp32-01", "esp32-cam"]
        self.calls = []
        self.bridge.gui_action = (
            lambda name, params, reason: self.calls.append((name, params))
            or {"ok": True, "msg": "ok"})

    # ---- parse_llm_reply ----
    def test_parse_valid_json(self):
        b = self.bridge
        r = b.parse_llm_reply(
            '{"action": "ping_device", "params": {"device_id": "esp32-01"},'
            ' "reason": "r", "reply": "pinged"}')
        self.assertEqual(r["action"], "ping_device")
        self.assertEqual(r["params"]["device_id"], "esp32-01")
        self.assertEqual(r["reply"], "pinged")

    def test_parse_repairs_raw_newline_in_string(self):
        b = self.bridge
        raw = '{"action": "none", "params": {}, "reason": "", "reply": "step 1\nstep 2"}'
        r = b.parse_llm_reply(raw)
        self.assertEqual(r["action"], "none")
        self.assertIn("step 1\nstep 2", r["reply"])

    def test_parse_fallback_non_json(self):
        b = self.bridge
        r = b.parse_llm_reply("just a plain text answer, no json here")
        self.assertEqual(r["action"], "none")
        self.assertIn("plain text answer", r["reply"])

    # ---- validate ----
    def test_validate_action_not_allowed(self):
        _, err = self.bridge.validate("reboot_universe", {}, ["esp32-01"])
        self.assertIn("not allowed", err)

    def test_validate_missing_required_param(self):
        _, err = self.bridge.validate("save_qos", {"priority": 5}, ["esp32-01"])
        self.assertIn("missing required", err)

    def test_validate_unknown_device(self):
        _, err = self.bridge.validate("save_qos",
                                      {"device_id": "ghost", "priority": 5},
                                      ["esp32-01"])
        self.assertIn("unknown device", err)

    def test_validate_clamps_params(self):
        params, err = self.bridge.validate(
            "save_qos", {"device_id": "esp32-01", "priority": 99,
                         "traffic_class": 4}, ["esp32-01"])
        self.assertIsNone(err)
        self.assertEqual(params["priority"], 7)
        self.assertEqual(params["traffic_class"], 3)

    # ---- handle_chat dispatch ----
    def test_chat_guide_none_executes_nothing(self):
        self._mock_chat({"action": "none", "params": {}, "reason": "",
                         "reply": "Use the QoS page, then VLAN."})
        res = self.bridge.handle_chat({"message": "how do I configure this?"})
        self.assertTrue(res["ok"])
        self.assertEqual(res["action"], "none")
        self.assertEqual(res["executed"], [])
        self.assertEqual(self.calls, [])

    def test_chat_valid_action_executes_and_auto_deploys(self):
        self._mock_chat({"action": "save_qos",
                         "params": {"device_id": "esp32-01", "priority": 6},
                         "reason": "r", "reply": "done"})
        res = self.bridge.handle_chat({"message": "set esp32-01 priority to 6"})
        self.assertTrue(res["ok"])
        names = [c[0] for c in self.calls]
        self.assertIn("save_qos", names)
        self.assertIn("exec_all", names)  # auto-deploy after a config change

    def test_chat_unknown_device_refused(self):
        self._mock_chat({"action": "save_qos",
                         "params": {"device_id": "ghost", "priority": 6},
                         "reason": "r", "reply": "done"})
        res = self.bridge.handle_chat({"message": "set ghost priority to 6"})
        self.assertFalse(res["ok"])
        self.assertIn("refused", res["msg"].lower())
        self.assertEqual(self.calls, [])

    def test_chat_prompt_forbids_executing_questions(self):
        # The strengthened prompt must keep the hard DO-vs-GUIDE rule intact.
        p = self.bridge.SYSTEM_PROMPT
        self.assertIn("a question is never an order", p)
        self.assertIn("action \"none\"", p)


if __name__ == "__main__":
    unittest.main()
