#!/usr/bin/env python3
"""WTSN Configurator — web GUI (Python stdlib only).

Serves the SQLite database written by the C configurator as an interactive web app.
Run:  python3 webgui.py  [port]
Open http://127.0.0.1:8000/
"""
import json
import os
import re
import sqlite3
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

BASE = os.path.dirname(os.path.abspath(__file__))
DB = os.environ.get("WTSN_DB", os.path.join(BASE, "build", "wtsn_gui.db"))
PORT = int(os.environ.get("WTSN_PORT", "8000"))

TABLES = ["devices", "device_tsn_features", "qos_configs", "vlan_groups",
          "vlan_members", "tas_schedules", "gcl_entries", "timesync_status",
          "sensors", "settings"]


def connect():
    con = sqlite3.connect(DB, timeout=3)
    con.row_factory = sqlite3.Row
    return con


def load_all():
    con = connect()
    out = {}
    try:
        for t in TABLES:
            try:
                out[t] = [dict(r) for r in con.execute(f"SELECT * FROM {t}")]
            except sqlite3.Error:
                out[t] = []
    finally:
        con.close()
    return out


def clamp(v, lo, hi):
    try:
        return max(lo, min(hi, int(v)))
    except (TypeError, ValueError):
        return lo


def act_devices(con, body):
    for i in body.get("delete") or []:
        con.execute("DELETE FROM devices WHERE id=?", (i,))
        con.execute("DELETE FROM qos_configs WHERE device_id=?", (i,))
        con.execute("DELETE FROM vlan_members WHERE device_id=?", (i,))
        con.execute("DELETE FROM sensors WHERE device_id=?", (i,))
        con.execute("DELETE FROM device_tsn_features WHERE device_id=?", (i,))
    dev = body.get("device") or {}
    if dev.get("id"):
        con.execute(
            "INSERT OR REPLACE INTO devices(id,name,ip,mac,kind,firmware,status,last_seen)"
            " VALUES(?,?,?,?,?,?,?,strftime('%s','now'))",
            (dev["id"], dev.get("name", ""), dev.get("ip", ""), dev.get("mac", ""),
             clamp(dev.get("kind", 0), 0, 3), dev.get("firmware", ""),
             clamp(dev.get("status", 0), 0, 2)))
        con.execute("DELETE FROM device_tsn_features WHERE device_id=?", (dev["id"],))
        for f in dev.get("tsn", []):
            con.execute("INSERT INTO device_tsn_features(device_id,feature) VALUES(?,?)",
                        (dev["id"], f))


def run_action(act, body):
    con = connect()
    try:
        if act == "save_devices":
            act_devices(con, body)
            con.commit()
            return {"ok": True, "msg": "Devices updated"}
        if act == "save_qos":
            con.execute(
                "INSERT OR REPLACE INTO qos_configs(device_id,priority,traffic_class,"
                "bandwidth_kbps,latency_ms,preemption) VALUES(?,?,?,?,?,?)",
                (body.get("device_id"), clamp(body.get("priority", 5), 0, 7),
                 clamp(body.get("traffic_class", 1), 0, 3),
                 clamp(body.get("bandwidth_kbps", 1000), 1, 1000000),
                 clamp(body.get("latency_ms", 1), 0, 10000),
                 clamp(body.get("preemption", 0), 0, 2)))
            con.commit()
            return {"ok": True, "msg": "QoS saved"}
        if act == "delete_qos":
            con.execute("DELETE FROM qos_configs WHERE device_id=?", (body.get("device_id"),))
            con.commit()
            return {"ok": True, "msg": "QoS deleted"}
        if act == "save_vlan":
            vlan = clamp(body.get("vlan_id", 1), 1, 4094)
            gid = body.get("id") or f"grp{vlan}"
            con.execute("INSERT OR REPLACE INTO vlan_groups(id,name,vlan_id) VALUES(?,?,?)",
                        (gid, body.get("name", ""), vlan))
            con.commit()
            return {"ok": True, "msg": "VLAN group saved"}
        if act == "delete_vlan":
            con.execute("DELETE FROM vlan_groups WHERE id=?", (body.get("id"),))
            con.execute("DELETE FROM vlan_members WHERE group_id=?", (body.get("id"),))
            con.commit()
            return {"ok": True, "msg": "VLAN group deleted"}
        if act == "save_member":
            con.execute("INSERT OR REPLACE INTO vlan_members(group_id,device_id) VALUES(?,?)",
                        (body.get("group_id"), body.get("device_id")))
            con.commit()
            return {"ok": True, "msg": "Member added"}
        if act == "delete_member":
            con.execute("DELETE FROM vlan_members WHERE group_id=? AND device_id=?",
                        (body.get("group_id"), body.get("device_id")))
            con.commit()
            return {"ok": True, "msg": "Member removed"}
        if act == "save_tas":
            cid = body.get("id") or f"sched{int(time.time())}"
            con.execute("INSERT OR REPLACE INTO tas_schedules(id,name,cycle_time_ns,"
                        "deploy_target) VALUES(?,?,?,?)",
                        (cid, body.get("name", ""), clamp(body.get("cycle_time_ns", 1000000), 0, 10**12),
                         body.get("deploy_target", "")))
            con.execute("DELETE FROM gcl_entries WHERE schedule_id=?", (cid,))
            for i, e in enumerate(body.get("gcl") or []):
                con.execute("INSERT INTO gcl_entries(schedule_id,index,gate_state,duration_ns)"
                           " VALUES(?,?,?,?)",
                           (cid, i, clamp(e.get("gate_state", 0), 0, 255),
                            clamp(e.get("duration_ns", 0), 0, 10**12)))
            con.commit()
            return {"ok": True, "msg": "TAS schedule saved"}
        if act == "delete_tas":
            con.execute("DELETE FROM tas_schedules WHERE id=?", (body.get("id"),))
            con.execute("DELETE FROM gcl_entries WHERE schedule_id=?", (body.get("id"),))
            con.commit()
            return {"ok": True, "msg": "TAS schedule deleted"}
        if act == "save_timesync":
            con.execute("INSERT OR REPLACE INTO timesync_status(id,mode,grandmaster,"
                        "offset_ns,quality) VALUES('main',?,?,?,?)",
                        (clamp(body.get("mode", 0), 0, 3), body.get("grandmaster", ""),
                         clamp(body.get("offset_ns", 0), -(10**12), 10**12),
                         clamp(body.get("quality", 0), 0, 100)))
            con.commit()
            return {"ok": True, "msg": "Time sync saved"}
        return {"ok": False, "msg": "unknown action: " + str(act)}
    except Exception as ex:
        return {"ok": False, "msg": str(ex)}
    finally:
        con.close()


def make_handler():
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, ctype, body):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            p = urlsplit(self.path).path
            if p == "/api/data":
                self._send(200, "application/json",
                          json.dumps(load_all()).encode())
            else:
                self._send(200, "text/html; charset=utf-8", HTML.encode("utf-8"))

        def do_POST(self):
            if urlsplit(self.path).path != "/api/action":
                self._send(404, "text/plain", b"nf")
                return
            n = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(n) or b"{}")
            except Exception:
                body = {}
            res = run_action(body.get("action"), body.get("data", body))
            self._send(200, "application/json", json.dumps(res).encode())

    return H


HTML = r"""<!doctype html><html><head><meta charset="utf-8">
<title>WTSN Configurator</title>
<style>
:root{--bg:#12121A;--surf:#1E1E28;--pri:#4C70FF;--sec:#2f9ee6;--text:#E8E8F0;--dim:#9A9AA8;--ok:#31C96B;--warn:#FFC94A;--err:#FF5F56;--border:#33333F}
*{box-sizing:border-box;font-family:system-ui,-apple-system,sans-serif}
body{margin:0;background:var(--bg);color:var(--text);display:flex;height:100vh}
nav{width:200px;background:var(--surf);padding:8px 0;flex-shrink:0;border-right:1px solid var(--border);overflow:auto}
nav button{display:block;width:100%;padding:11px;background:none;border:none;border-bottom:1px solid var(--border);color:var(--text);text-align:left;font-size:14px;cursor:pointer}
nav button:hover{background:var(--pri)} nav button.active{background:var(--pri)}
main{flex:1;padding:20px;overflow:auto}
.card{background:var(--surf);border:1px solid var(--border);border-radius:8px;padding:10px;margin-bottom:12px}
.card h3{margin:0 0 8px}
.row{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin:5px 0}
label{color:var(--dim);font-size:12px;width:110px;flex-shrink:0}
input,select{background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:4px;padding:5px}
input[type=number]{width:90px} input[type=text]{flex:1;min-width:120px}
button.act{background:var(--pri);color:white;border:none;border-radius:4px;padding:7px 12px;cursor:pointer;margin-top:6px;margin-right:6px}
button.act.danger{background:var(--err)} button.act.ghost{background:none;color:var(--dim);border:1px solid var(--border)}
.dim{color:var(--dim)} .ok{color:var(--ok)} .err{color:var(--err)} .warn{color:var(--warn)}
table{width:100%;border-collapse:collapse;font-size:13px;margin-top:6px}
th,td{border-bottom:1px solid var(--border);padding:6px;text-align:left} th{color:var(--dim);font-weight:600}
#toast{position:fixed;bottom:20px;right:20px;background:var(--surf);border:1px solid var(--sec);color:var(--text);padding:10px 14px;border-radius:8px;display:none;z-index:99}
.statgrid{display:flex;gap:14px;flex-wrap:wrap;margin-bottom:14px}
.stat{flex:1;min-width:140px;padding:14px;border-radius:8px;border:1px solid var(--border);background:var(--surf);text-align:center}
.stat .n{font-size:28px;color:var(--ok)}
.gcl-table{font-size:12px}
</style></head><body>
<nav id="nav"></nav>
<main id="main"></main>
<div id="toast"></div>
<script>
const PAGES=["dashboard","devices","qos","vlan","tas","timesync","sensors","settings"];
let DATA={};

async function getData(){DATA=await (await fetch('/api/data')).json();renderNav();go('dashboard');}
function renderNav(){const n=document.getElementById('nav');n.innerHTML='';
 PAGES.forEach(p=>{const b=document.createElement('button');b.textContent=p;b.onclick=()=>go(p);b.id='nav_'+p;n.appendChild(b);});}
function setActive(p){document.querySelectorAll('#nav button').forEach(b=>b.classList.remove('active'));
 document.getElementById('nav_'+p)?.classList.add('active');}
function toast(m,cls){const t=document.getElementById('toast');t.style.display='block';t.className=cls||'';t.textContent=m;
 clearTimeout(t._h);t._h=setTimeout(()=>t.style.display='none',2200);}
function esc(s){return String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
function V(id){return document.getElementById(id)?.value||'';}

async function post(act,data){const r=await fetch('/api/action',{method:'POST',
 headers:{'Content-Type':'application/json'},body:JSON.stringify({action:act,data})});
 const j=await r.json();toast(j.msg,j.ok?'ok':'err');if(j.ok)await getData();}

/* ---------- dashboard ---------- */
function go(p){setActive(p);const m=document.getElementById('main');
 const views={dashboard:renderDashboard,devices:renderDevices,qos:renderQos,
  vlan:renderVlan,tas:renderTas,timesync:renderTimesync,sensors:renderSensors,settings:renderSettings};
 m.innerHTML=views[p](); if(p==='dashboard')afterDash();
 if(typeof window['bind_'+p]==='function')window['bind_'+p]();}

function renderDashboard(){
 const online=DATA.devices.filter(d=>d.status==0).length;
 const vlanCount=DATA.vlan_groups.length, tasCount=DATA.tas_schedules.length;
 return '<div class="statgrid">'
 +stat('Devices online',online)+stat('Devices total',DATA.devices.length)
 +stat('VLAN groups',vlanCount)+stat('TAS schedules',tasCount)
 +'</div><div class="card"><h3>System</h3>'
 +'OPC UA server with <b>real PubSub</b> (UADP/UDP multicast 239.255.0.1:4840) + MQTT bridge active.<br>'
 +'The C configurator persists state to SQLite; this web page reads and edits the same DB.<br>'
 +'Simulation mode: run <code>tsn-node-simulator --all</code> for virtual nodes.</div>'
 +'<div class="card"><h3>Quick actions</h3>'
 +'<button class="act" onclick="post(\'timesync_sim\',{})">No-op</button></div>';
}
function stat(l,v){return '<div class="stat"><div class="dim">'+l+'</div><div class="n">'+v+'</div></div>';}
function afterDash(){}

/* ---------- devices ---------- */
function renderDevices(){
 let rows=DATA.devices.map(d=>`<tr><td>${esc(d.id)}</td><td>${esc(d.name)}</td>
 <td class="${d.status==0?'ok':d.status==2?'err':'dim'}">${['online','offline','error'][d.status]}</td>
 <td>${esc(d.ip)}</td><td>${esc(d.firmware)}</td>
 <td>${esc((DATA.device_tsn_features||[]).filter(f=>f.device_id==d.id).map(f=>f.feature).join(', '))}</td>
 <td><button class="act ghost" onclick="loadDev('${esc(d.id)}')">edit</button></td></tr>`).join('');
 return `<div class="card"><h3>Add / edit device</h3>
 <div class="row"><label>id</label><input id="did"></div>
 <div class="row"><label>name</label><input id="dname"></div>
 <div class="row"><label>ip</label><input id="dip"></div>
 <div class="row"><label>mac</label><input id="dmac"></div>
 <div class="row"><label>firmware</label><input id="dfw"></div>
 <div class="row"><label>kind</label><select id="dkind"><option value="0">Generic</option>
 <option value="1">ESP32</option><option value="2">Raspberry Pi</option><option value="3">STM32</option></select></div>
 <div class="row"><label>tsn features</label><input id="dtsn" placeholder="comma separated: 802.1Qav,802.1Qbv"></div>
 <button class="act" onclick="saveDevice()">Add / Update</button></div>
 <h3>Devices (${DATA.devices.length})</h3>
 <table><tr><th>id</th><th>name</th><th>status</th><th>ip</th><th>fw</th><th>tsn</th><th></th></tr>${rows}</table>`;
}
function loadDev(id){const d=DATA.devices.find(x=>x.id==id);if(!d)return;
 V('did')||0; document.getElementById('did').value=d.id;
 document.getElementById('dname').value=d.name||'';
 document.getElementById('dip').value=d.ip||'';
 document.getElementById('dmac').value=d.mac||'';
 document.getElementById('dfw').value=d.firmware||'';
 document.getElementById('dkind').value=d.kind||0;
 document.getElementById('dtsn').value=(DATA.device_tsn_features||[]).filter(f=>f.device_id==id).map(f=>f.feature).join(',');
}
function saveDevice(){post('save_devices',{device:{id:V('did'),name:V('dname'),ip:V('dip'),
 mac:V('dmac'),firmware:V('dfw'),kind:parseInt(V('dkind')||'0',10),status:0,
 tsn:V('dtsn').split(',').map(s=>s.trim()).filter(Boolean)}});}

/* ---------- qos ---------- */
function renderQos(){
 let rows=DATA.qos_configs.map(q=>`<tr><td>${esc(q.device_id)}</td><td>${q.priority}</td>
 <td>${q.traffic_class}</td><td>${q.bandwidth_kbps} kbps</td><td>${q.latency_ms} ms</td>
 <td>${esc(q.preemption)}</td><td><button class="act ghost" onclick="loadQos('${esc(q.device_id)}')">edit</button>
 <button class="act ghost" onclick="post('delete_qos',{device_id:'${esc(q.device_id)}'})">delete</button></td></tr>`).join('');
 let opts=DATA.devices.map(d=>`<option value="${esc(d.id)}">${esc(d.id)}</option>`).join('');
 return `<div class="card"><h3>Configure QoS (IEEE 802.1Q) for device</h3>
 <div class="row"><label>device</label><select id="qdev"><option value="">—</option>${opts}</select></div>
 <div class="row"><label>priority 0-7</label><input id="qprio" type="number" min="0" max="7" value="5"></div>
 <div class="row"><label>traffic class</label><select id="qtc"><option value="1">Audio/Video</option>
 <option value="2">Controlled load</option><option value="3">Critical</option><option value="0">Best effort</option></select></div>
 <div class="row"><label>bandwidth kbps</label><input id="qbw" type="number" min="1" value="1000"></div>
 <div class="row"><label>latency ms</label><input id="qlat" type="number" min="0" value="5"></div>
 <div class="row"><label>preemption</label><select id="qpre"><option value="0">Off</option>
 <option value="1">Express queue</option><option value="2">On</option></select></div>
 <button class="act" onclick="saveQos()">Save QoS</button></div>
 <h3>QoS configs (${DATA.qos_configs.length})</h3>
 <table><tr><th>device</th><th>prio</th><th>tc</th><th>bw</th><th>latency</th><th>preempt</th><th></th></tr>${rows}</table>`;
}
function loadQos(id){const q=DATA.qos_configs.find(x=>x.device_id==id);if(!q)return;
 document.getElementById('qdev').value=id;document.getElementById('qprio').value=q.priority;
 document.getElementById('qtc').value=q.traffic_class;document.getElementById('qbw').value=q.bandwidth_kbps;
 document.getElementById('qlat').value=q.latency_ms;document.getElementById('qpre').value=q.preemption;}
function saveQos(){post('save_qos',{device_id:V('qdev'),priority:parseInt(V('qprio')),
 traffic_class:parseInt(V('qtc')),bandwidth_kbps:parseInt(V('qbw')),
 latency_ms:parseInt(V('qlat')),preemption:parseInt(V('qpre'))});}

/* ---------- vlan ---------- */
function renderVlan(){
 let rows=DATA.vlan_groups.map(g=>{
  const members=DATA.vlan_members.filter(mg=>mg.group_id==g.id).map(x=>x.device_id).join(', ');
  return `<tr><td>${esc(g.id)}</td><td>${esc(g.name)}</td><td>${g.vlan_id}</td>
  <td>${esc(members)}</td>
  <td><button class="act ghost" onclick="loadVlan('${esc(g.id)}')">edit</button>
  <button class="act ghost" onclick="post('delete_vlan',{id:'${esc(g.id)}'})">delete</button></td></tr>`}).join('');
 let devs=DATA.devices.map(d=>`<option value="${esc(d.id)}">${esc(d.id)}</option>`).join('');
 return `<div class="card"><h3>Add VLAN group</h3>
 <div class="row"><label>id</label><input id="vid"><span class="dim">optional</span></div>
 <div class="row"><label>name</label><input id="vname"></div>
 <div class="row"><label>vlan id</label><input id="vvlan" type="number" min="1" max="4094" value="100"></div>
 <button class="act" onclick="saveVlan()">Add group</button></div>
 <div class="card"><h3>Add member to group</h3>
 <div class="row"><label>group</label><select id="mg">${DATA.vlan_groups.map(g=>`<option value="${esc(g.id)}">${esc(g.id)}</option>`).join('')}</select></div>
 <div class="row"><label>device</label><select id="md">${devs}</select></div>
 <button class="act" onclick="addMember()">Add member</button></div>
 <h3>VLAN groups (${DATA.vlan_groups.length})</h3>
 <table><tr><th>id</th><th>name</th><th>vlan</th><th>members</th><th></th></tr>${rows}</table>`;
}
function loadVlan(id){const g=DATA.vlan_groups.find(x=>x.id==id);if(!g)return;
 document.getElementById('vid').value=g.id;document.getElementById('vname').value=g.name;
 document.getElementById('vvlan').value=g.vlan_id;}
function saveVlan(){post('save_vlan',{id:V('vid'),name:V('vname'),vlan_id:parseInt(V('vvlan'))});}
function addMember(){post('save_member',{group_id:V('mg'),device_id:V('md')});}

/* ---------- tas ---------- */
function renderTas(){
 let blocks=DATA.tas_schedules.map(s=>{
  const gcl=DATA.gcl_entries.filter(e=>e.schedule_id==s.id).map(e=>
   `<tr><td>${e.index}</td><td>${e.gate_state}</td><td>${e.duration_ns} ns</td></tr>`).join('');
  return `<div class="card"><h3>${esc(s.name)}</h3>
  <div class="row"><label>id</label><span>${esc(s.id)}</span></div>
  <div class="row"><label>cycle</label><span>${s.cycle_time_ns} ns</span></div>
  <div class="row"><label>deploy target</label><span>${esc(s.deploy_target)}</span></div>
  <table class="gcl-table"><tr><th>#</th><th>gate_state</th><th>duration_ns</th></tr>${gcl}</table>
  <button class="act ghost" onclick="loadTas('${esc(s.id)}')">edit</button>
  <button class="act danger" onclick="post('delete_tas',{id:'${esc(s.id)}'})">Delete</button></div>`}).join('');
 let devs=DATA.devices.map(d=>`<option value="${esc(d.id)}">${esc(d.id)}</option>`).join('');
 return `<div class="card"><h3>New TAS schedule (802.1Qbv)</h3>
 <div class="row"><label>id</label><input id="tid"><span class="dim">optional</span></div>
 <div class="row"><label>name</label><input id="tname"></div>
 <div class="row"><label>cycle ns</label><input id="tcycle" type="number" value="1000000"></div>
 <div class="row"><label>deploy target</label><select id="tdev"><option value="">—</option>${devs}</select></div>
 <div class="row"><label>GCL</label><input id="tgcl" value="1:400000,0:600000"
  title="entries as gate_state:duration_ns, comma separated"></div>
 <button class="act" onclick="saveTas()">Save schedule</button></div>
 <h3>TAS schedules (${DATA.tas_schedules.length})</h3>${blocks}`;
}
function loadTas(id){const s=DATA.tas_schedules.find(x=>x.id==id);if(!s)return;
 document.getElementById('tid').value=s.id;document.getElementById('tname').value=s.name;
 document.getElementById('tcycle').value=s.cycle_time_ns;document.getElementById('tdev').value=s.deploy_target;
 const g=DATA.gcl_entries.filter(e=>e.schedule_id==id).map(e=>e.gate_state+':'+e.duration_ns).join(',');
 document.getElementById('tgcl').value=g;}
function saveTas(){const g=V('tgcl').split(',').map(x=>{const [a,b]=x.split(':');return{gate_state:parseInt(a),duration_ns:parseInt(b)}}).filter(e=>!isNaN(e.gate_state));
 post('save_tas',{id:V('tid'),name:V('tname'),cycle_time_ns:parseInt(V('tcycle')),
 deploy_target:V('tdev'),gcl:g});}

/* ---------- timesync ---------- */
function renderTimesync(){
 const t=DATA.timesync_status[0]||{};
 const modes=['disabled','local grandmaster','external grandmaster','auto'];
 return `<div class="card"><h3>Time Sync (gPTP / IEEE 802.1AS)</h3>
 <div class="row"><label>mode</label><select id="tsmode">${modes.map((m,i)=>`<option value="${i}" ${(t.mode||0)==i?'selected':''}>${m}</option>`).join('')}</select></div>
 <div class="row"><label>grandmaster</label><input id="tsgm" value="${esc(t.grandmaster||'')}"></div>
 <div class="row"><label>offset ns</label><input id="tsoff" type="number" value="${t.offset_ns||0}"></div>
 <div class="row"><label>quality</label><input id="tsqual" type="number" min="0" max="100" value="${t.quality||0}"></div>
 <button class="act" onclick="saveTs()">Save time sync</button></div>`;
}
function saveTs(){post('save_timesync',{mode:parseInt(V('tsmode')),grandmaster:V('tsgm'),
 offset_ns:parseInt(V('tsoff')),quality:parseInt(V('tsqual'))});}

/* ---------- sensors ---------- */
function renderSensors(){
 if(!DATA.sensors.length)return '<div class="card">No sensors. Run the node simulator or add devices with sensors.</div>';
 let rows=DATA.sensors.map(s=>`<tr><td>${esc(s.device_id)}</td><td>${esc(s.sensor_id)}</td>
 <td>${esc(s.type)}</td><td>${esc(s.name)}</td><td>${s.value} ${esc(s.unit)}</td>
 <td class="${s.healthy?'ok':'err'}">${s.healthy?'healthy':'fault'}</td></tr>`).join('');
 return `<h3>Sensors (${DATA.sensors.length})</h3>
 <table><tr><th>device</th><th>id</th><th>type</th><th>name</th><th>value</th><th>health</th></tr>${rows}</table>`;
}

/* ---------- settings ---------- */
function renderSettings(){return `<div class="card"><h3>Settings</h3>
 <p>Database: <code>${esc(document.location.hostname)}</code></p>
 <p>This web GUI reads and edits the same SQLite database the C configurator uses.
 Configurations below are persisted and pushed to nodes (simulated or real).</p>
 <p>Refresh a page or click a nav button to reload data.</p></div>`;}
getData();</script></body></html>
"""


def main():
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    srv = ThreadingHTTPServer(("127.0.0.1", port), make_handler())
    print(f"WTSN web GUI: http://127.0.0.1:{port}  (db={DB})")
    try:
        import webbrowser
        webbrowser.open(f"http://127.0.0.1:{port}/")
    except Exception:
        pass
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
