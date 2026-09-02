"""Simulation engine: fabricates devices, sensors and a realistic frame flow."""
import random
import threading
import time

from . import state
from .db import add_event, connect

PROFILES = [(0, "esp32", "192.168.1.10", "ESP32 Gateway"),
            (2, "rpi", "192.168.1.20", "Raspberry Pi"),
            (0, "linux", "192.168.1.30", "Linux Node"),
            (3, "stm32", "192.168.1.40", "STM32 Sensor"),
            (0, "nxp", "192.168.1.50", "NXP Node")]
TSN_FUNCS = ["802.1Q QoS", "802.1Q VLAN", "gPTP 802.1AS", "802.1Qbv TAS",
             "802.1Qbu Preemption", "OPC UA", "OPC UA PubSub", "FX Multicast"]

SIM_STABLE_DEVICES = None
SIM_STABLE_LOCK = threading.Lock()


def _gen_stable_devices():
    """Generate a fixed simulated device set once. Reused every tick so the device
    list stays constant while sensor values / status continue to drift."""
    devs = []
    n = random.randint(3, 6)
    per = {}
    for i in range(1, n + 1):
        kind, base, ip, name = random.choice(PROFILES)
        per.setdefault(base, 0)
        per[base] += 1
        did = "%s-%02d" % (base, per[base])
        devs.append({
            "id": did, "name": name, "ip": ip, "mac": "AA:BB:CC:%02d:%02d" % (i, kind),
            "kind": kind, "firmware": "%d.%d.%d" % (random.randint(1, 5),
                      random.randint(0, 9), random.randint(0, 9)),
            "tsn": random.sample(TSN_FUNCS, random.randint(4, len(TSN_FUNCS))),
        })
    return devs


def sim_tick():
    global SIM_STABLE_DEVICES
    con = connect()
    try:
        with state.SIM_USER_DEVICES_LOCK:
            keep = list(state.SIM_USER_DEVICES)
        kept = {}
        for k in keep:
            row = con.execute("SELECT * FROM devices WHERE id=?", (k,)).fetchone()
            if row:
                kept[k] = dict(row)
            else:
                with state.SIM_USER_DEVICES_LOCK:
                    state.SIM_USER_DEVICES.discard(k)
        saved_ts = con.execute("SELECT * FROM timesync_status WHERE id='main'").fetchone()
        saved_ts = dict(saved_ts) if saved_ts else None
        saved_nodes = con.execute("SELECT * FROM settings WHERE key='sync_nodes'").fetchone()
        saved_nodes = dict(saved_nodes) if saved_nodes else None
        # Config tables are user configuration and must NOT be regenerated/destroyed by
        # the simulator. Only simulated runtime data is rebuilt each tick, using a stable
        # simulated device set (generated once) so devices do not appear/disappear each tick.
        for t in ["devices", "sensors", "timesync_status",
                  "device_tsn_features"]:
            con.execute("DELETE FROM %s" % t)
        for k, kv in kept.items():
            con.execute("INSERT OR REPLACE INTO devices(id,name,ip,mac,kind,firmware,status,"
                        "last_seen,domain) VALUES(?,?,?,?,?,?,?,?,?)",
                        (k, kv.get("name", ""), kv.get("ip", ""), kv.get("mac", ""),
                         kv.get("kind", 0), kv.get("firmware", ""), kv.get("status", 0),
                         kv.get("last_seen", int(time.time())), kv.get("domain", "default")))
        with SIM_STABLE_LOCK:
            if SIM_STABLE_DEVICES is None:
                SIM_STABLE_DEVICES = _gen_stable_devices()
            stable = list(SIM_STABLE_DEVICES)
        devs = [d["id"] for d in stable]
        for sd in stable:
            did = sd["id"]
            con.execute("INSERT INTO devices(id,name,ip,mac,kind,firmware,status,last_seen,domain)"
                       " VALUES(?,?,?,?,?,?,0,strftime('%s','now'),'default')",
                       (did, sd["name"], sd["ip"], sd["mac"], sd["kind"],
                        sd["firmware"]))
            for f in sd["tsn"]:
                con.execute("INSERT INTO device_tsn_features(device_id,feature) VALUES(?,?)",
                            (did, f))
            for sid, typ, unit, basev in (("temp1", 0, "C", 25.0), ("press1", 1, "hPa", 1005.0),
                                           ("imu1", 2, "g", 0.3), ("gpio1", 4, "V", 1.0)):
                con.execute("INSERT OR REPLACE INTO sensors(device_id,sensor_id,type,name,"
                            "value,unit,healthy,last_update) VALUES(?,?,?,?,?,?,1,strftime('%s','now'))",
                            (did, sid, typ, sid, basev, unit))
        gm = devs[0] if devs else "esp32-01"
        if saved_ts and saved_ts["grandmaster"]:
            gm = saved_ts["grandmaster"]
            con.execute("INSERT OR REPLACE INTO timesync_status(id,mode,grandmaster,offset_ns,quality)"
                       " VALUES('main',?,?,?,?)", (saved_ts["mode"], saved_ts["grandmaster"],
                                                    saved_ts["offset_ns"], saved_ts["quality"]))
        else:
            con.execute("INSERT OR REPLACE INTO timesync_status(id,mode,grandmaster,offset_ns,quality)"
                        " VALUES('main',1,?,?,?)", (gm, random.randint(-50, 500),
                                                     random.randint(80, 99)))
        if saved_nodes and saved_nodes["value"]:
            con.execute("INSERT OR REPLACE INTO settings(key,value) VALUES('sync_nodes',?)",
                       (saved_nodes["value"],))
        con.commit()
        gm_ip = "192.168.1.%d" % random.randint(2, 50)
        add_event("discovery", "cnc", "nodes announced, %d nodes on network" % len(devs),
                  src_ip="192.168.1.%d" % random.randint(2, 50), dst_ip=gm_ip, dest=gm)
        add_event("mqtt", gm, "telemetry {'temp':%s,'press':%s}" % (
                 random.randint(150, 350) / 10, random.randint(990, 1020)),
                 src_ip=gm_ip, dst_ip="192.168.1.%d" % random.randint(2, 50), dest=gm)
        slave = random.choice(devs) if devs else gm
        slave_ip = "192.168.1.%d" % random.randint(2, 50)
        gmid = "80:00:11:ff:fe:%02x:%02x:01" % (random.randint(0, 255), random.randint(0, 255))
        cid = "00:1b:%02x:%02x:%02x:%02x:80:01" % tuple(random.randint(0, 255) for _ in range(4))
        syncd = random.random() < 0.75
        if random.random() < 0.85:
            add_event("ptp", gm, "BMCA: %s elected grandmaster, GM ID %s" % (gm, gmid),
                     src_ip=gm_ip, dst_ip=slave_ip, dest=slave, proto="IEEE 802.1AS")
        if syncd:
            add_event("ptp", gm, "Sync (ClockIdentity %s)" % cid,
                     src_ip=gm_ip, dst_ip=slave_ip, dest=slave, proto="IEEE 802.1AS")
            add_event("ptp", gm, "Follow_Up (precision timestamp)",
                     src_ip=gm_ip, dst_ip=slave_ip, dest=slave, proto="IEEE 802.1AS")
            add_event("ptp", slave, "Delay_Req to master", src_ip=slave_ip,
                     dst_ip=gm_ip, dest=gm, proto="IEEE 802.1AS")
            off = random.randint(-500, 500)
            add_event("ptp", gm, "Delay_Resp: offset from master %d ns (GM ID %s)" %
                     (off, gmid), src_ip=gm_ip, dst_ip=slave_ip, dest=slave,
                     proto="IEEE 802.1AS")
            add_event("ptp", gm, "Sync jitter %d ns" % random.randint(0, 200),
                     src_ip=gm_ip, dst_ip=slave_ip, dest=slave, proto="IEEE 802.1AS")
        add_event("ptp", gm, "gPTP %s offset %d ns, jitter %d ns" %
                  ("in sync" if syncd else "out of sync",
                   random.randint(-300, 300), random.randint(0, 250)),
                 src_ip=gm_ip, dst_ip=slave_ip, dest=gm, proto="IEEE 802.1AS")
        add_event("qos", "cnc", "802.1Q priority 5 traffic class 3 deployed",
                 src_ip="192.168.1.%d" % random.randint(2, 50), dst_ip=gm_ip, dest=gm, proto="IEEE 802.1Q (QoS)")
        add_event("vlan", "cnc", "vlan_id 100 group Control deployed",
                 src_ip="192.168.1.%d" % random.randint(2, 50), dst_ip=gm_ip, dest=gm, proto="IEEE 802.1Q (WVLAN)")
        if random.random() < 0.7:
            add_event("tas", "cnc", "TAS/GCL schedule active, cycle 1 ms",
                     src_ip="192.168.1.%d" % random.randint(2, 50), dst_ip=gm_ip, dest=gm, proto="IEEE 802.1Qbv")
        if random.random() < 0.5:
            add_event("pre", "cnc", "preemption eMAC [7,6,5] pMAC [3,2,1,0]",
                     src_ip="192.168.1.%d" % random.randint(2, 50), dst_ip=gm_ip, dest=gm, proto="IEEE 802.1Qbu")
        add_event("fx", gm, "FX over MQTT C2C field exchange", src_ip=gm_ip,
                 dst_ip="192.168.1.255", dest="")
    finally:
        con.close()


def sim_runner():
    while True:
        try:
            if state.MODE["mode"] == "sim":
                sim_tick()
        except Exception as ex:
            add_event("error", "sim", str(ex))
        time.sleep(2.5)
