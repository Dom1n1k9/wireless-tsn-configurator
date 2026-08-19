#!/usr/bin/env python3
"""WTSN Configurator - web GUI (Python stdlib only).

Deployment vs Simulation mode, devices with TSN functions and grandmaster/slave
role, QoS (802.1Q, priority 0-7), VLAN (ID), TAS/GCL (802.1Qbv), gPTP
(802.1AS), sensors, OPC UA, PubSub, MQTT, OPC UA FX/wireless multicast, and a
live network/frame monitor. In simulation mode a background thread fabricates
devices, sensors and a realistic frame flow so everything can be exercised without HW.

Run:  python3 webgui.py [port]   ->  http://127.0.0.1:8000/
"""
import json, os, random, sqlite3, threading, time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

BASE = os.path.dirname(os.path.abspath(__file__))
DB_REAL = os.environ.get("WTSN_DB", os.path.join(BASE, "build", "wtsn_gui.db"))
DB_SIM = os.path.join(BASE, "build", "wtsn_sim.db")
PORT = int(os.environ.get("WTSN_PORT", "8000"))

TABLES = ["devices", "device_tsn_features", "qos_configs", "vlan_groups",
          "vlan_members", "tas_schedules", "gcl_entries", "timesync_status",
          "sensors", "settings"]

SCHEMA = (
    "CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY,name TEXT,ip TEXT,mac TEXT,"
    "kind INTEGER,firmware TEXT,status INTEGER,last_seen INTEGER);"
    "CREATE TABLE IF NOT EXISTS device_tsn_features(device_id TEXT,feature TEXT);"
    "CREATE TABLE IF NOT EXISTS qos_configs(device_id TEXT,priority INTEGER,traffic_class "
    "INTEGER,bandwidth_kbps INTEGER,latency_ms INTEGER,preemption INTEGER);"
    "CREATE TABLE IF NOT EXISTS vlan_groups(id TEXT PRIMARY KEY,name TEXT,vlan_id INTEGER);"
    "CREATE TABLE IF NOT EXISTS vlan_members(group_id TEXT,device_id TEXT);"
    "CREATE TABLE IF NOT EXISTS tas_schedules(id TEXT PRIMARY KEY,name TEXT,cycle_time_ns "
    "INTEGER,deploy_target TEXT);"
    "CREATE TABLE IF NOT EXISTS gcl_entries(schedule_id TEXT,\"index\" INTEGER,gate_state "
    "INTEGER,duration_ns INTEGER);"
    "CREATE TABLE IF NOT EXISTS timesync_status(id TEXT,mode INTEGER,grandmaster TEXT,"
    "offset_ns INTEGER,quality INTEGER);"
    "CREATE TABLE IF NOT EXISTS sensors(device_id TEXT,sensor_id TEXT,type INTEGER,name TEXT,"
    "value REAL,unit TEXT,healthy INTEGER,last_update INTEGER);"
    "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT);"
)

def ensure_schema(con):
    con.executescript(SCHEMA)
    con.commit()

PROFILES = [(0, "esp32", "192.168.1.10", "ESP32 Gateway"),
            (2, "rpi", "192.168.1.20", "Raspberry Pi"),
            (0, "linux", "192.168.1.30", "Linux Node"),
            (3, "stm32", "192.168.1.40", "STM32 Sensor"),
            (0, "nxp", "192.168.1.50", "NXP Node")]
TSN_FUNCS = ["802.1Q QoS", "802.1Q VLAN", "gPTP 802.1AS", "802.1Qbv TAS",
             "802.1Qbu Preemption", "OPC UA", "OPC UA PubSub", "FX Multicast"]

EVENTS = deque(maxlen=400)
EVENT_LOCK = threading.Lock()
MODE = {"mode": "sim"}


def connect():
    con = sqlite3.connect(DB_SIM if MODE["mode"] == "sim" else DB_REAL, timeout=3)
    con.row_factory = sqlite3.Row
    ensure_schema(con)
    return con


def add_event(kind, source, data):
    with EVENT_LOCK:
        EVENTS.appendleft({"ts": time.strftime("%H:%M:%S"), "kind": kind,
                         "source": source, "data": data})


def load_all():
    out = {}
    con = connect()
    try:
        for t in TABLES:
            try:
                out[t] = [dict(r) for r in con.execute("SELECT * FROM %s" % t)]
            except sqlite3.Error:
                out[t] = []
    finally:
        con.close()
    return out


def get_events():
    with EVENT_LOCK:
        return {"mode": MODE["mode"], "events": list(EVENTS)[:300]}


def sim_tick():
    con = connect()
    try:
        for t in ["devices", "qos_configs", "vlan_groups", "vlan_members",
                  "tas_schedules", "gcl_entries", "timesync_status", "sensors",
                  "device_tsn_features"]:
            con.execute("DELETE FROM %s" % t)
        n = random.randint(3, 6)
        devs = []
        for i in range(1, n + 1):
            kind, base, ip, name = random.choice(PROFILES)
            did = "%s-%02d" % (base, i)
            con.execute("INSERT INTO devices(id,name,ip,mac,kind,firmware,status,last_seen)"
                        " VALUES(?,?,?,?,?,?,0,strftime('%s','now'))",
                        (did, name, ip, "AA:BB:CC:%02d:%02d" % (i, kind), kind,
                         "%d.%d.%d" % (random.randint(1, 5), random.randint(0, 9),
                                           random.randint(0, 9))))
            for f in random.sample(TSN_FUNCS, random.randint(4, len(TSN_FUNCS))):
                con.execute("INSERT INTO device_tsn_features(device_id,feature) VALUES(?,?)",
                            (did, f))
            cid = "grp%d" % (i % 3 + 1)
            con.execute("INSERT OR IGNORE INTO vlan_groups(id,name,vlan_id) VALUES(?,?,?)",
                        (cid, ["Control", "Media", "Bulk"][i % 3], 100 + i * 100))
            con.execute("INSERT OR IGNORE INTO vlan_members(group_id,device_id) VALUES(?,?)",
                        (cid, did))
            con.execute("INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
                        "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?)",
                        (did, random.randint(0, 7), random.randint(0, 3),
                         random.randint(1000, 50000), random.randint(1, 8),
                         random.randint(0, 2)))
            for sid, typ, unit in (("temp1", 0, "C"), ("press1", 1, "hPa"),
                                   ("imu1", 2, "g"), ("gpio1", 4, "V")):
                if random.random() < 0.7:
                    val = (round(random.uniform(15, 35), 1) if typ == 0 else
                           round(random.uniform(990, 1020), 1) if typ == 1 else
                           round(random.uniform(0, 0.5), 2))
                    con.execute("INSERT OR REPLACE INTO sensors(device_id,sensor_id,type,name,"
                                "value,unit,healthy,last_update) VALUES(?,?,?,?,?,?,1,strftime('%s','now'))",
                                (did, sid, typ, sid, val, unit))
            devs.append(did)
        gm = devs[0] if devs else "esp32-01"
        con.execute("INSERT OR REPLACE INTO timesync_status(id,mode,grandmaster,offset_ns,quality)"
                    " VALUES('main',1,?,?,?)", (gm, random.randint(-50, 500),
                                                 random.randint(80, 99)))
        con.execute("INSERT OR REPLACE INTO tas_schedules(id,name,cycle_time_ns,deploy_target)"
                    " VALUES(?,?,?,?)", ("sched1", "Control cycle", 1000000, gm))
        con.execute("DELETE FROM gcl_entries WHERE schedule_id='sched1'")
        for idx, (g, d) in enumerate([(0x01, 300000), (0x03, 200000), (0x00, 500000)]):
            con.execute("INSERT INTO gcl_entries(schedule_id,\"index\",gate_state,duration_ns)"
                        " VALUES('sched1',?,?,?)", (idx, g, d))
        con.commit()
        for did in devs:
            add_event("mqtt", did, "tsn/nodes/%s  <-  JSON telemetry" % did)
        for did in devs[:2]:
            add_event("pubsub", gm, "UADP DataSet %s -> 239.255.0.1:4840" % did)
            add_event("frame", did, "raw %s" % "".join(
                random.choice("0123456789ABCDEF") for _ in range(28)))
        if random.random() < 0.6:
            add_event("fx", gm, "OPC UA FX multicast 239.255.0.1:4840")
        if random.random() < 0.4:
            add_event("config", "cnc", "schema deployed to " + gm)
        add_event("discovery", gm, "nodes announced")
    finally:
        con.close()


def sim_runner():
    while True:
        try:
            if MODE["mode"] == "sim":
                sim_tick()
        except Exception as ex:
            add_event("error", "sim", str(ex))
        time.sleep(2.5)


def clamp(v, lo, hi):
    try:
        return max(lo, min(hi, int(v)))
    except (TypeError, ValueError):
        return lo


def run_action(act, body):
    con = connect()
    try:
        if act == "set_mode":
            MODE["mode"] = "sim" if body.get("mode") == "sim" else "real"
            EVENTS.clear()
            add_event("config", "cnc",
                     "mode = SIMULATION" if MODE["mode"] == "sim" else "mode = REAL (waiting for real devices)")
            return {"ok": True, "msg": "mode = " + MODE["mode"]}
        if act == "save_devices":
            for i in body.get("delete") or []:
                for t, c in (("devices", "id"), ("qos_configs", "device_id"),
                             ("vlan_members", "device_id"), ("sensors", "device_id"),
                             ("device_tsn_features", "device_id")):
                    con.execute("DELETE FROM %s WHERE %s=?" % (t, c), (i,))
                add_event("config", "cnc", "removed " + i)
            dev = body.get("device") or {}
            if dev.get("id"):
                con.execute("INSERT OR REPLACE INTO devices(id,name,ip,mac,kind,firmware,status,"
                            "last_seen) VALUES(?,?,?,?,?,?,?,strftime('%s','now'))",
                            (dev["id"], dev.get("name", ""), dev.get("ip", ""),
                             dev.get("mac", ""), clamp(dev.get("kind", 0), 0, 3),
                             dev.get("firmware", ""), clamp(dev.get("status", 0), 0, 2)))
                con.execute("DELETE FROM device_tsn_features WHERE device_id=?", (dev["id"],))
                for f in dev.get("tsn", []) or []:
                    con.execute("INSERT INTO device_tsn_features(device_id,feature) VALUES(?,?)",
                                (dev["id"], f))
                add_event("config", "cnc", "device %s updated: %s" % (dev["id"],
                         ",".join(dev.get("tsn") or [])))
            con.commit()
            return {"ok": True, "msg": "Devices updated"}
        if act == "set_role":
            role = body.get("role")
            did = body.get("id")
            if role == "grandmaster":
                con.execute("UPDATE timesync_status SET grandmaster=? WHERE id='main'", (did,))
                con.commit()
            add_event("config", "cnc", "%s -> %s" % (did, role))
            return {"ok": True, "msg": "role set"}
        if act == "save_qos":
            con.execute("INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
                        "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?)",
                        (body.get("device_id"), clamp(body.get("priority", 5), 0, 7),
                         clamp(body.get("traffic_class", 1), 0, 3),
                         clamp(body.get("bandwidth_kbps", 1000), 1, 1000000),
                         clamp(body.get("latency_ms", 1), 0, 10000),
                         clamp(body.get("preemption", 0), 0, 2)))
            con.commit()
            add_event("config", "cnc", "QoS prio %s -> %s" % (body.get("priority"),
                     body.get("device_id")))
            return {"ok": True, "msg": "QoS saved"}
        if act == "delete_qos":
            con.execute("DELETE FROM qos_configs WHERE device_id=?", (body.get("device_id"),))
            con.commit()
            return {"ok": True, "msg": "QoS deleted"}
        if act == "save_vlan":
            vlan = clamp(body.get("vlan_id", 1), 1, 4094)
            gid = body.get("id") or ("grp%d" % vlan)
            con.execute("INSERT OR REPLACE INTO vlan_groups(id,name,vlan_id) VALUES(?,?,?)",
                        (gid, body.get("name", ""), vlan))
            con.commit()
            add_event("config", "cnc", "VLAN %s id %d" % (gid, vlan))
            return {"ok": True, "msg": "VLAN group saved"}
        if act == "delete_vlan":
            con.execute("DELETE FROM vlan_groups WHERE id=?", (body.get("id"),))
            con.execute("DELETE FROM vlan_members WHERE group_id=?", (body.get("id"),))
            con.commit()
            return {"ok": True, "msg": "VLAN deleted"}
        if act == "save_member":
            con.execute("INSERT OR REPLACE INTO vlan_members(group_id,device_id) VALUES(?,?)",
                        (body.get("group_id"), body.get("device_id")))
            con.commit()
            return {"ok": True, "msg": "Member added"}
        if act == "save_tas":
            cid = body.get("id") or ("sched%d" % int(time.time()))
            con.execute("INSERT OR REPLACE INTO tas_schedules(id,name,cycle_time_ns,deploy_target)"
                        " VALUES(?,?,?,?)",
                        (cid, body.get("name", ""), clamp(body.get("cycle_time_ns", 1000000), 0, 10**12),
                         body.get("deploy_target", "")))
            con.execute("DELETE FROM gcl_entries WHERE schedule_id=?", (cid,))
            for i, e in enumerate(body.get("gcl") or []):
                con.execute("INSERT INTO gcl_entries(schedule_id,index,gate_state,duration_ns)"
                            " VALUES(?,?,?,?)",
                            (cid, i, clamp(e.get("gate_state", 0), 0, 255),
                             clamp(e.get("duration_ns", 0), 0, 10**12)))
            con.commit()
            add_event("config", "cnc", "TAS %s -> %s" % (cid, body.get("deploy_target")))
            return {"ok": True, "msg": "TAS saved"}
        if act == "delete_tas":
            con.execute("DELETE FROM tas_schedules WHERE id=?", (body.get("id"),))
            con.execute("DELETE FROM gcl_entries WHERE schedule_id=?", (body.get("id"),))
            con.commit()
            return {"ok": True, "msg": "TAS deleted"}
        if act == "save_timesync":
            con.execute("INSERT OR REPLACE INTO timesync_status(id,mode,grandmaster,offset_ns,"
                        "quality) VALUES('main',?,?,?,?)",
                        (clamp(body.get("mode", 0), 0, 3), body.get("grandmaster", ""),
                         clamp(body.get("offset_ns", 0), -(10**12), 10**12),
                         clamp(body.get("quality", 0), 0, 100)))
            con.commit()
            return {"ok": True, "msg": "Time sync saved"}
        if act == "fx_send":
            add_event("fx", body.get("source", "cnc"), "FX multicast 239.255.0.1:4840: " +
                     body.get("msg", ""))
            return {"ok": True, "msg": "FX sent"}
        if act == "clear_events":
            EVENTS.clear()
            return {"ok": True, "msg": "monitor cleared"}
        return {"ok": False, "msg": "unknown action"}
    except Exception as ex:
        return {"ok": False, "msg": str(ex)}
    finally:
        con.close()


def make_handler():
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, b):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_GET(self):
            p = urlsplit(self.path).path
            if p == "/api/data":
                d = load_all()
                d["mode"] = MODE["mode"]
                with EVENT_LOCK:
                    d["events"] = list(EVENTS)[:300]
                self._send(json.dumps(d).encode())
            elif p == "/api/events":
                self._send(json.dumps(get_events()).encode())
            else:
                body = HTML.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        def do_POST(self):
            if urlsplit(self.path).path != "/api/action":
                self.send_error(404)
                return
            try:
                n = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(n) or b"{}")
            except Exception:
                body = {}
            res = run_action(body.get("action"), body.get("data", body))
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            b = json.dumps(res).encode()
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

    return H


HTML = r"""<!doctype html><html><head><meta charset="utf-8">
<title>WTSN Configurator</title>
<style>
:root{--bg:#0e0e16;--surf:#161622;--surf2:#1e1e2c;--pri:#4C8DFF;--sec:#2f9ee6;--ok:#31C96B;--warn:#FFC94A;--err:#FF5F56;--text:#E8E8F0;--dim:#8a8aa0;--border:#2a2a3a}
*{box-sizing:border-box;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
body{margin:0;background:var(--bg);color:var(--text);height:100vh;display:flex;flex-direction:column;overflow:hidden}
header{display:flex;align-items:center;gap:14px;padding:0 16px;height:54px;background:var(--surf);border-bottom:1px solid var(--border);flex-shrink:0}
header .logo{font-weight:700;font-size:16px;color:var(--pri)}
header .sub{color:var(--dim);font-size:12px}
.spacer{flex:1}
.mode{display:flex;border:1px solid var(--border);border-radius:8px;overflow:hidden}
.mode button{padding:7px 14px;background:none;color:var(--dim);border:none;cursor:pointer;font-size:13px}
.mode button.on{background:var(--pri);color:#fff}
.wrap{display:flex;flex:1;overflow:hidden}
nav{width:180px;background:var(--surf);border-right:1px solid var(--border);padding:8px 0;overflow:auto;flex-shrink:0}
nav button{display:block;width:100%;padding:11px 14px;background:none;border:none;color:var(--dim);text-align:left;font-size:14px;cursor:pointer}
nav button.on{color:var(--text);background:var(--surf2);border-left:3px solid var(--pri)}
main{flex:1;overflow:auto;padding:18px}
h2{margin:0 0 12px;font-size:18px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px;margin-bottom:16px}
.stat{border:1px solid var(--border);border-radius:10px;padding:14px;background:var(--surf)}
.stat .l{color:var(--dim);font-size:12px}
.stat .v{font-size:26px;margin-top:4px;color:var(--ok)}
.card{border:1px solid var(--border);border-radius:10px;background:var(--surf);padding:14px;margin-bottom:14px}
.card h3{margin:0 0 10px;font-size:15px}
.row{display:flex;gap:8px;align-items:center;margin:6px 0;flex-wrap:wrap}
label{color:var(--dim);font-size:12px;width:120px;flex-shrink:0}
input,select{background:var(--surf2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:6px}
input[type=number]{width:100px}
button{background:var(--pri);color:#fff;border:none;border-radius:6px;padding:7px 12px;cursor:pointer}
button.danger{background:#8a0000}button.ghost{background:none;color:var(--dim);border:1px solid var(--border)}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{border-bottom:1px solid var(--border);padding:6px;text-align:left}th{color:var(--dim)}
#toast{position:fixed;bottom:18px;right:18px;background:var(--surf2);border:1px solid var(--sec);padding:10px 14px;border-radius:8px;display:none;z-index:50}
.monrow{display:flex;gap:10px;padding:4px 0;border-bottom:1px solid var(--border);font-family:ui-monospace,monospace;font-size:12px}
.monrow .t{color:var(--dim);width:70px}.monrow .s{color:var(--sec);width:110px}
</style></head><body>
<header><span class="logo">WTSN Configurator</span><span class="sub">Wireless TSN control plane</span>
<div class="spacer"></div>
<span class="mode"><button id="md_real" onclick="setMode('real')">Real</button><button id="md_sim" class="on" onclick="setMode('sim')">Simulation</button></span>
</header>
<div class="wrap"><nav id="nav"></nav><main id="main"></main></div>
<div id="toast"></div>
<script>
const PAGES=["dashboard","devices","qos","vlan","tas","timesync","opcua","pubsub_mqtt","fx","monitor","sensors","settings"];
let D={};
function $(id){return document.getElementById(id)}
function esc(s){return String(s==null?"":s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]))}
async function api(a,d){const r=await fetch("/api/action",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({action:a,data:d})});const j=await r.json();toast(j.msg,j.ok);return j}
function toast(m,ok){const t=$("toast");t.style.display="block";t.style.borderColor=ok?"var(--sec)":"var(--err)";t.textContent=m;clearTimeout(t._h);t._h=setTimeout(()=>t.style.display="none",2200)}
async function load(){const r=await fetch("/api/data");D=await r.json();renderNav();go("dashboard")}
function renderNav(){const n=$("nav");n.innerHTML="";
 PAGES.forEach(p=>{const b=document.createElement("button");b.textContent=p;b.onclick=()=>go(p);b.id="nv_"+p;n.appendChild(b)});
 $("md_real").className=D.mode==="real"?"on":"";$("md_sim").className=D.mode==="sim"?"on":""}
function setMode(m){api("set_mode",{mode:m}).then(load)}
function stat(l,v){return "<div class='stat'><div class='l'>"+l+"</div><div class='v'>"+v+"</div></div>"}
function types(d){return (D.device_tsn_features||[]).filter(f=>f.device_id==d).map(f=>f.feature).join(", ")}
function tsnOpts(gm){return D.devices.map(d=>"<label><input type=checkbox value='"+esc(d.id)+"' "+(d.id==gm?"checked":"")+"> GM:"+esc(d.id)+"</label>").join("")}
function go(p){document.querySelectorAll("#nav button").forEach(b=>b.className=b.id==="nv_"+p?"on":"");
 const m=$("main");
 if(p==="dashboard")m.innerHTML=dash();
 else if(p==="devices")m.innerHTML=devices();
 else if(p==="qos")m.innerHTML=qos();
 else if(p==="vlan")m.innerHTML=vlan();
 else if(p==="tas")m.innerHTML=tas();
 else if(p==="timesync")m.innerHTML=timesync();
 else if(p==="opcua")m.innerHTML=opcua();
 else if(p==="pubsub_mqtt")m.innerHTML=pubsub_mqtt();
 else if(p==="fx")m.innerHTML=fx();
 else if(p==="monitor"){m.innerHTML=monitor();refreshMon();}
 else if(p==="sensors")m.innerHTML=sensors();
 else if(p==="settings")m.innerHTML=settings()}
function dash(){const on=D.devices.filter(d=>d.status==0).length;
 return "<h2>Dashboard</h2><div class='grid'>"+stat("Devices",D.devices.length)+stat("Online",on)+
 stat("VLAN",D.vlan_groups.length)+stat("TAS",D.tas_schedules.length)+
 stat("Sensors",D.sensors.length)+stat("Mode",D.mode)+"</div>"+
 "<div class='card'><h3>Network</h3>OPC UA server (real PubSub UADP/UDP multicast 239.255.0.1:4840) + MQTT bridge.<br>"+
 (D.mode==="sim"?"In <b>Simulation</b> mode you can configure and see live frame flow without hardware.":
 "In <b>Real</b> mode the configurator waits for real TSN nodes (ESP32/RPi/STM32/NXP/Linux) and their sensors.")+"</div>"}
function devices(){let rows=D.devices.map(d=>"<tr><td>"+esc(d.id)+"</td><td>"+esc(d.name)+"</td><td class='"+(d.status==0?"":"")+"'>"+["online","offline","error"][d.status]||d.status+"</td><td>"+esc(d.ip)+"</td><td>"+esc(d.firmware)+"</td><td>"+esc(types(d.id))+"</td><td><button class='ghost' onclick='loadDev(\""+esc(d.id)+"\")'>edit</button></td></tr>").join("");
 let opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 return "<h2>Devices</h2><div class='card'><h3>Add / edit TSN device</h3>"+
 "<div class='row'><label>id</label><input id=did><span class='row'><label>name</label><input id=dname></span></div>"+
 "<div class='row'><label>ip</label><input id=dip><span class='row'><label>firmware</label><input id=dfw></span></div>"+
 "<div class='row'><label>kind</label><select id=dkind><option value=0>Generic</option><option value=1>ESP32</option><option value=2>Raspberry Pi</option><option value=3>STM32</option></select></div>"+
 "<h3>TSN functions</h3>"+["802.1Q QoS","802.1Q VLAN","gPTP 802.1AS","802.1Qbv TAS","802.1Qbu Preemption","OPC UA","OPC UA PubSub","FX Multicast"].map(f=>"<label><input type=checkbox id=fn_"+esc(f.split(' ')[0].replace(/[^A-Za-z0-9]/g,''))+"> "+f+"</label>").join("")+
 "<div><button onclick='saveDev()'>Save</button></div></div>"+
 "<h3>Grandmaster / Slaves</h3><div class='card'><div class='row'><label>set as GM</label><select id=gm_sel><option value=''>-</option>"+opts+"</select>"+
 "<button onclick=gmSet()>Apply</button></div></div>"+
 "<h3>Devices</h3><table><tr><th>id</th><th>name</th><th>status</th><th>ip</th><th>fw</th><th>tsn</th><th></th></tr>"+rows+"</table>"}
function fnIds(){const map={"802":["802","802","gPTP","802","802","OPC","OPC","FX"]};return ["802","802","gPTP","802","802","OPC","OPC","FX"]}
function loadDev(id){const d=D.devices.find(x=>x.id==id);if(!d)return;$("did").value=d.id;$("dname").value=d.name;$("dip").value=d.ip;$("dfw").value=d.firmware;$("dkind").value=d.kind}
function saveDev(){const fns=fnIds().map((k,i)=>document.getElementById("fn_"+k)?document.getElementById("fn_"+k).checked:false);
 const tsn=["802.1Q QoS","802.1Q VLAN","gPTP 802.1AS","802.1Qbv TAS","802.1Qbu Preemption","OPC UA","OPC UA PubSub","FX Multicast"].filter((x,i)=>fns[i]);
 api("save_devices",{device:{id:$("did").value,name:$("dname").value,ip:$("dip").value,firmware:$("dfw").value,kind:parseInt($("dkind").value),status:0,tsn:tsn}})}
function gmSet(){const sel=$("gm_sel");if(sel.value)api("set_role",{id:sel.value,role:"grandmaster"})}
function qos(){let rows=D.qos_configs.map(q=>"<tr><td>"+esc(q.device_id)+"</td><td>"+q.priority+"</td><td>"+q.bandwidth_kbps+"</td><td>"+q.latency_ms+" ms</td><td>"+q.preemption+"</td></tr>").join("");
 const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 return "<h2>QoS (IEEE 802.1Q) — priority 0-7</h2><div class='card'><div class='row'><label>device</label><select id=q_dev><option value=''>-</option>"+opts+"</select></div>"+
 "<div class='row'><label>priority 0-7</label><input id=q_prio type=number min=0 max=7 value=5></div>"+
 "<div class='row'><label>bandwidth kbps</label><input id=q_bw type=number value=1000></div>"+
 "<div class='row'><label>latency ms</label><input id=q_lat type=number value=2></div>"+
 "<div class='row'><label>traffic class</label><select id=q_tc><option value=1>Audio/Video</option><option value=2>Controlled</option><option value=3>Critical</option><option value=0>Best effort</option></select></div>"+
 "<div class='row'><label>preemption</label><select id=q_pre><option value=0>off</option><option value=1>express</option><option value=2>on</option></select></div>"+
 "<button onclick=saveQ()>Save QoS</button></div><table><tr><th>device</th><th>prio</th><th>bw</th><th>lat</th><th>pre</th></tr>"+rows+"</table>"}
function saveQ(){api("save_qos",{device_id:$("q_dev").value,priority:parseInt($("q_prio").value),traffic_class:0,bandwidth_kbps:parseInt($("q_bw").value),latency_ms:parseInt($("q_lat").value),preemption:parseInt($("q_pre").value)})}
function vlan(){let rows=D.vlan_groups.map(g=>"<tr><td>"+esc(g.id)+"</td><td>"+esc(g.name)+"</td><td>"+g.vlan_id+"</td><td>"+esc((D.vlan_members||[]).filter(x=>x.group_id==g.id).map(x=>x.device_id).join(", "))+"</td></tr>").join("");
 return "<h2>VLAN (IEEE 802.1Q)</h2><div class='card'><div class='row'><label>name</label><input id=v_name></div><div class='row'><label>VLAN ID 1-4094</label><input id=v_id type=number value=100></div><button onclick='api(\"save_vlan\",{id:\"v_\"+Math.random().toString(36).slice(2,6),name:$(\"v_name\").value,vlan_id:parseInt($(\"v_id\").value)})'>Add group</button></div><table><tr><th>id</th><th>name</th><th>vlan</th><th>members</th></tr>"+rows+"</table>"}
function tas(){let bl=D.tas_schedules.map(s=>{const g=(D.gcl_entries||[]).filter(e=>e.schedule_id==s.id).map(e=>e.gate_state+":"+e.duration_ns).join(", ");return "<div class='card'><h3>"+esc(s.name)+"</h3><div class='row'><label>id</label><span>"+esc(s.id)+"</span></div><div class='row'><label>cycle</label><span>"+s.cycle_time_ns+" ns</span></div><div class='row'><label>deploy</label><span>"+esc(s.deploy_target)+"</span></div><div class='row'><label>GCL</label><span>"+esc(g)+"</span></div></div>"}).join("");
 const opts=D.devices.map(d=>"<option value='"+esc(d.id)+"'>"+esc(d.id)+"</option>").join("");
 return "<h2>TAS (IEEE 802.1Qbv) — Gate Control List</h2><div class='card'><div class='row'><label>name</label><input id=t_name></div><div class='row'><label>cycle ns</label><input id=t_cyc type=number value=1000000></div><div class='row'><label>deploy target</label><select id=t_dev>"+opts+"</select></div><div class='row'><label>GCL</label><input id=t_gcl value='1:300000,3:200000,0:500000'></div><button onclick=tSave()>Save / Deploy</button></div>"+bl}
function tSave(){const g=$("t_gcl").value.split(",").map(x=>{const[a,b]=x.split(":");return{gate_state:parseInt(a),duration_ns:parseInt(b)}});api("save_tas",{id:"s",name:$("t_name").value||"schedule",cycle_time_ns:parseInt($("t_cyc").value),deploy_target:$("t_dev").value,gcl:g})}
function timesync(){const t=D.timesync_status[0]||{};const modes=["disabled","local GM","external GM","auto"];
 return "<h2>Time Sync (gPTP / IEEE 802.1AS)</h2><div class='card'><div class='row'><label>mode</label><select id=ts_mode>"+modes.map((m,i)=>["<option value='"+i+"' "+(String(t.mode)==String(i)?"selected":"")+">"+m+"</option>"].join("")).join("")+"</select></div>"+
 "<div class='row'><label>grandmaster</label><input id=ts_gm value='"+esc(t.grandmaster||"")+"'></div>"+
 "<div class='row'><label>offset ns</label><input id=ts_off type=number value='"+(t.offset_ns||0)+"'></div>"+
 "<div class='row'><label>quality</label><input id=ts_q type=number value='"+(t.quality||0)+"'></div>"+
 "<button onclick='api(\"save_timesync\",{mode:parseInt($(\"ts_mode\").value),grandmaster:$(\"ts_gm\").value,offset_ns:parseInt($(\"ts_off\").value),quality:parseInt($(\"ts_q\").value)})'>Save</button></div>"}
function opcua(){return "<h2>OPC UA Server</h2><div class='card'><div class='row'><label>port</label><span>4840</span></div><div class='row'><label>namespace</label><span>urn:wtsn:configurator (ns 2)</span></div><div class='row'><label>endpoint</label><span>opc.tcp://127.0.0.1:4840</span></div><div class='row'><label>status</label><span class='"+(D.mode==="sim"?"'":"'")+"'>"+(D.mode==="sim"?"simulated":"listening — waiting for real OPC UA devices")+"</span></div></div>"}
function pubsub_mqtt(){return "<h2>PubSub & MQTT</h2><div class='card'><h3>OPC UA PubSub</h3><div class='row'><label>backend</label><span>UADP / UDP</span></div><div class='row'><label>dataset</label><span>wtsnData (max 64 fields)</span></div><h3>MQTT client</h3><div class='row'><label>broker</label><span>127.0.0.1:1883</span></div><div class='row'><label>topic</label><span>tsn/#</span></div><div class='row'><label>MQTT -> OPC UA</label><span>gateway maps topics to OPC UA paths</span></div></div>"}
function fx(){return "<h2>OPC UA FX / Wireless Multicast</h2><div class='card'><div class='row'><label>group</label><span>239.255.0.1:4840 (UA-DP)</span></div><div class='row'><label>transport</label><span>all W-TSN members join group, no broker</span></div><div class='row'><label>message</label><input id=fx_msg value='alert'></div><button onclick='api(\"fx_send\",{msg:$(\"fx_msg\").value})'>Send FX multicast</button></div>"}
function monitor(){return "<h2>Network / Frame Monitor</h2><div class='card'><div class='row'><span>"+(D.mode==="sim"?"Live simulated flow (MQTT, OPC UA PubSub, FX multicast, raw frames)":"Real traffic — waiting for real nodes")+"</span>"+"<span class='spacer'></span><button class='ghost' onclick='api(\"clear_events\",{})'>Clear</button></div><div id=mon></div></div>"}
function refreshMon(){if($("mon")){const ev=(D.events||[]).slice(0,120).map(e=>"<div class='monrow'><span class='t'>"+esc(e.ts)+"</span><span class='s'>"+esc(e.source)+"</span><span>"+esc(e.data)+"</span></div>").join("");$("mon").innerHTML=ev||"<div class='monrow'><span>no traffic yet</span></div>"}}
function sensors(){let r=D.sensors.map(s=>"<tr><td>"+esc(s.device_id)+"</td><td>"+esc(s.sensor_id)+"</td><td>"+esc(s.type)+"</td><td>"+s.value+" "+esc(s.unit)+"</td><td class='"+(s.healthy?"":"') style='color:var(--err)")+"'>"+(s.healthy?"healthy":"fault")+"</td></tr>").join("");
 return "<h2>Sensors ("+(D.mode==="real"?"real":"simulated")+")</h2>"+(D.sensors.length?("<table><tr><th>device</th><th>id</th><th>type</th><th>value</th><th>health</th></tr>"+r+"</table>"):"<div class='card'>"+(D.mode==="sim"?"Waiting for simulation sensors...":"Connect real sensors — none identified yet.")+"</div>")}
function settings(){return "<h2>Settings</h2><div class='card'><div class='row'><label>mode</label><span>"+D.mode+"</span></div><div class='row'><label>db</label><span>"+esc(D.db||"wtsn_gui.db")+"</span></div><div class='row'><label>push</label><span>configs are stored to SQLite; CLI/simulator apply them to nodes</span></div></div>"}
setInterval(async()=>{const r=await fetch("/api/events");const j=await r.json();if(j.mode){D.mode=j.mode}if($("mon")){const ev=(j.events||[]).slice(0,120).map(e=>"<div class='monrow'><span class='t'>"+esc(e.ts)+"</span><span class='s'>"+esc(e.source)+"</span><span>"+esc(e.data)+"</span></div>").join("");$("mon").innerHTML=ev||""}},2000);
load();</script></body></html>
"""


if __name__ == "__main__":
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    threading.Thread(target=sim_runner, daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", port), make_handler())
    print("WTSN web GUI: http://127.0.0.1:%d  (db=%s)" % (port, DB), flush=True)
    try:
        import webbrowser
        webbrowser.open("http://127.0.0.1:%d/" % port)
    except Exception:
        pass
    srv.serve_forever()
