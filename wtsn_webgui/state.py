"""Shared mutable state for the web GUI process (guarded by the locks here)."""
import os
import threading
from collections import deque

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB_DIR = os.path.join(BASE, "build")
os.makedirs(DB_DIR, exist_ok=True)
DB_REAL = os.environ.get("WTSN_DB", os.path.join(DB_DIR, "wtsn_gui.db"))
DB_SIM = os.path.join(DB_DIR, "wtsn_sim.db")
FW_DIR = os.environ.get("WTSN_FW_DIR", os.path.join(DB_DIR, "fw"))
os.makedirs(FW_DIR, exist_ok=True)
PORT = int(os.environ.get("WTSN_PORT", "8000"))

TABLES = ["devices", "device_tsn_features", "qos_configs", "vlan_groups",
          "vlan_members", "tas_schedules", "gcl_entries", "timesync_status",
          "sensors", "preemption_configs", "tsn_streams", "tsn_stream_members",
          "settings", "domains", "config_versions"]

MODE = {"mode": "sim"}
EVENTS = deque(maxlen=400)
EVENT_LOCK = threading.Lock()
LISTENER_STOP = threading.Event()
WS_NOTIFY = threading.Event()   # set whenever state changes; the WS hub picks it up
RECENT_ACKS = {}
ACK_LOCK = threading.Lock()
SIM_USER_DEVICES = set()
SIM_USER_DEVICES_LOCK = threading.Lock()
OFFLINE_AFTER = 20
