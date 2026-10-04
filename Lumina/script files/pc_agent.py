#!/usr/bin/env python3
"""
PC agent: CPU/RAM/Disk/Net stats ke JSON hisebe serve kore (http://PC_IP:8080/stats)
Install:  pip install psutil
Run:      python pc_agent.py
Windows firewall-e port 8080 allow koro (private network).
"""
import json
import platform
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import shutil
import subprocess
import urllib.request

import psutil

PORT = 8080
state = {}
lock = threading.Lock()


def cpu_name():
    try:
        if platform.system() == "Linux":
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if line.startswith("model name"):
                        return line.split(":", 1)[1].strip()
        elif platform.system() == "Windows":
            import subprocess
            out = subprocess.check_output(
                ["wmic", "cpu", "get", "name"], text=True, stderr=subprocess.DEVNULL
            )
            lines = [l.strip() for l in out.splitlines() if l.strip()]
            if len(lines) > 1:
                return lines[1]
        elif platform.system() == "Darwin":
            import subprocess
            return subprocess.check_output(
                ["sysctl", "-n", "machdep.cpu.brand_string"], text=True
            ).strip()
    except Exception:
        pass
    return platform.processor() or "CPU"


def cpu_temp():
    try:
        temps = psutil.sensors_temperatures()
    except Exception:
        return -1
    for key in ("coretemp", "k10temp", "zenpower", "cpu_thermal", "acpitz"):
        if key in temps and temps[key]:
            return round(temps[key][0].current, 1)
    for entries in temps.values():
        if entries:
            return round(entries[0].current, 1)
    return -1  # Windows-e psutil temp dey na (LibreHardwareMonitor lagbe)


def disk_percent():
    path = "C:\\" if platform.system() == "Windows" else "/"
    return psutil.disk_usage(path).percent


# ---------- slow stats: process list, GPU, CPU temp on Windows ----------
slow = {
    "gpu": -1, "gpu_temp": -1, "gpu_mem_used": 0, "gpu_mem_total": 0,
    "procs": [], "nproc": 0, "cpu_temp": -1,
    "drives": [], "swap": 0, "bat": -1, "plug": False,
    "win": "", "wapp": "", "rprocs": [], "dprocs": [],
}
slow_lock = threading.Lock()


def nvidia():
    if not shutil.which("nvidia-smi"):
        return {}
    try:
        out = subprocess.check_output(
            ["nvidia-smi",
             "--query-gpu=temperature.gpu,utilization.gpu,memory.used,memory.total",
             "--format=csv,noheader,nounits"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        )
        t, u, mu, mt = [float(x) for x in out.strip().splitlines()[0].split(",")]
        return {"gpu_temp": t, "gpu": u,
                "gpu_mem_used": round(mu / 1024, 1), "gpu_mem_total": round(mt / 1024, 1)}
    except Exception:
        return {}


def _num(v):
    try:
        return float(str(v).split()[0].replace(",", "."))
    except Exception:
        return None


def lhm():
    """LibreHardwareMonitor Remote Web Server (http://127.0.0.1:8085) theke temp pore."""
    try:
        with urllib.request.urlopen("http://127.0.0.1:8085/data.json", timeout=1) as r:
            tree = json.load(r)
    except Exception:
        return {}
    f = {"temps": {}}

    def walk(n):
        text, val = n.get("Text", ""), n.get("Value", "")
        x = _num(val) if val else None
        if x is not None:
            if "°C" in val:
                f["temps"].setdefault(text, x)
            elif "%" in val and text == "GPU Core":
                f.setdefault("gpu", x)
            elif text in ("GPU Memory Used", "GPU Memory Total"):
                mb = x * 1024 if "GB" in val else x
                f.setdefault(text, mb)
        for c in n.get("Children") or []:
            walk(c)

    walk(tree)
    out = {}
    for k in ("CPU Package", "Core (Tctl/Tdie)", "CPU (Tctl/Tdie)", "Tctl/Tdie",
              "Core Max", "Core Average"):
        if k in f["temps"]:
            out["cpu_temp"] = f["temps"][k]
            break
    if "GPU Core" in f["temps"]:
        out["gpu_temp"] = f["temps"]["GPU Core"]
    if "gpu" in f:
        out["gpu"] = f["gpu"]
    if "GPU Memory Used" in f:
        out["gpu_mem_used"] = round(f["GPU Memory Used"] / 1024, 1)
    if "GPU Memory Total" in f:
        out["gpu_mem_total"] = round(f["GPU Memory Total"] / 1024, 1)
    return out


def ascii_only(s, n):
    """TFT font sudhu ASCII dekhay, tai non-ASCII bad."""
    return (s or "").encode("ascii", "ignore").decode().strip()[:n]


def active_window():
    """Ekhon kon window/app samne ache (Windows only)."""
    if platform.system() != "Windows":
        return "", ""
    try:
        import ctypes
        from ctypes import wintypes
        u32 = ctypes.windll.user32
        hwnd = u32.GetForegroundWindow()
        if not hwnd:
            return "", ""
        n = u32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(n + 1)
        u32.GetWindowTextW(hwnd, buf, n + 1)
        pid = wintypes.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        app = psutil.Process(pid.value).name() if pid.value else ""
        return ascii_only(buf.value, 50), ascii_only(app, 22)
    except Exception:
        return "", ""


def drives():
    out = []
    try:
        for p in psutil.disk_partitions(all=False):
            if "cdrom" in p.opts or not p.fstype or p.fstype in ("squashfs", "tmpfs"):
                continue
            if p.mountpoint.startswith(("/snap", "/boot", "/var/lib")):
                continue
            try:
                u = psutil.disk_usage(p.mountpoint)
            except Exception:
                continue
            name = p.mountpoint.rstrip("\\/") or p.mountpoint
            out.append({"n": ascii_only(name, 6) or "/", "p": round(u.percent),
                        "u": round(u.used / 2**30, 1), "t": round(u.total / 2**30)})
    except Exception:
        pass
    return out[:4]


def slow_sampler():
    ncpu = psutil.cpu_count() or 1
    io_prev = {}
    t_prev = time.time()
    while True:
        try:
            now_t = time.time()
            dt = max(now_t - t_prev, 0.2)
            t_prev = now_t
            agg, n, io_now = {}, 0, {}
            for p in psutil.process_iter(["pid", "name", "cpu_percent", "memory_info", "io_counters"]):
                try:
                    i = p.info
                    n += 1
                    if i["pid"] == 0:  # System Idle Process
                        continue
                    a = agg.setdefault(ascii_only(i["name"], 27) or "?", [0.0, 0.0, 0.0])
                    a[0] += (i["cpu_percent"] or 0) / ncpu
                    if i["memory_info"]:
                        a[1] += i["memory_info"].rss / 1048576
                    io = i.get("io_counters")
                    if io:
                        tot = io.read_bytes + io.write_bytes
                        io_now[i["pid"]] = tot
                        prev = io_prev.get(i["pid"])
                        if prev is not None and tot >= prev:
                            a[2] += (tot - prev) / 1048576 / dt
                except psutil.NoSuchProcess:
                    continue
            io_prev = io_now
            top = sorted(agg.items(), key=lambda kv: (kv[1][0], kv[1][1]), reverse=True)[:8]
            by_ram = sorted(agg.items(), key=lambda kv: kv[1][1], reverse=True)[:4]
            by_io = [kv for kv in sorted(agg.items(), key=lambda kv: kv[1][2], reverse=True)[:4]
                     if kv[1][2] > 0.01]
            res = {
                "procs": [{"n": k, "c": round(v[0], 1), "m": int(v[1])} for k, v in top],
                "rprocs": [{"n": k, "m": int(v[1])} for k, v in by_ram],
                "dprocs": [{"n": k, "r": round(v[2], 1)} for k, v in by_io],
                "nproc": n,
            }
            l = lhm()
            g = nvidia()
            res["cpu_temp"] = l.get("cpu_temp", -1)
            for k in ("gpu", "gpu_temp", "gpu_mem_used", "gpu_mem_total"):
                res[k] = g.get(k, l.get(k, slow[k]))
            res["drives"] = drives()
            try:
                res["swap"] = round(psutil.swap_memory().percent)
            except Exception:
                res["swap"] = 0
            try:
                b = psutil.sensors_battery()
                res["bat"] = round(b.percent) if b else -1
                res["plug"] = bool(b.power_plugged) if b else False
            except Exception:
                res["bat"], res["plug"] = -1, False
            res["win"], res["wapp"] = active_window()
            with slow_lock:
                slow.update(res)
        except Exception:
            pass
        time.sleep(2)


def sampler():
    name = cpu_name()
    host = socket.gethostname()[:20]
    psutil.cpu_percent(percpu=True)  # prime
    last = psutil.net_io_counters()
    last_d = psutil.disk_io_counters()
    last_t = time.time()
    while True:
        time.sleep(1)
        cores = psutil.cpu_percent(percpu=True)
        total = sum(cores) / len(cores) if cores else 0
        now = psutil.net_io_counters()
        t = time.time()
        dt = max(t - last_t, 0.001)
        up = (now.bytes_sent - last.bytes_sent) / 1024 / dt
        down = (now.bytes_recv - last.bytes_recv) / 1024 / dt
        last, last_t = now, t
        nd = psutil.disk_io_counters()
        if nd and last_d:
            dr = max((nd.read_bytes - last_d.read_bytes) / 1048576 / dt, 0)
            dw = max((nd.write_bytes - last_d.write_bytes) / 1048576 / dt, 0)
        else:
            dr = dw = 0.0
        last_d = nd

        vm = psutil.virtual_memory()
        freq = psutil.cpu_freq()
        data = {
            "host": host,
            "name": name,
            "cpu": round(total, 1),
            "cores": [round(c, 0) for c in cores],
            "freq": round(freq.current) if freq else 0,
            "temp": cpu_temp(),
            "ram": round(vm.percent, 1),
            "ram_used": round(vm.used / 1024**3, 1),
            "ram_total": round(vm.total / 1024**3, 1),
            "disk": round(disk_percent(), 1),
            "up": round(up, 1),
            "down": round(down, 1),
            "uptime": int(time.time() - psutil.boot_time()),
            "dr": round(dr, 2),
            "dw": round(dw, 2),
            "time": time.strftime("%H:%M:%S"),
            "date": time.strftime("%a %d %b"),
        }
        with slow_lock:
            extra = dict(slow)
        if data["temp"] <= 0 and extra.get("cpu_temp", -1) > 0:
            data["temp"] = round(extra["cpu_temp"], 1)
        data.update({k: v for k, v in extra.items() if k != "cpu_temp"})
        with lock:
            state.clear()
            state.update(data)


DASH_HTML = """<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>PC Monitor</title><style>
body{margin:0;background:#0b0f14;color:#e6edf3;font-family:system-ui,sans-serif}
.w{max-width:900px;margin:auto;padding:16px}
.g{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}
.c{background:#151b23;border-radius:12px;padding:14px}
.big{font-size:56px;font-weight:700;line-height:1}
.l{color:#8b98a5;font-size:13px;margin-bottom:6px}
.bar{height:12px;background:#222b36;border-radius:6px;overflow:hidden;margin:6px 0 10px}
.bar i{display:block;height:100%;width:0;background:#3fb950;transition:width .4s}
#cores{display:flex;gap:4px;height:90px;align-items:flex-end}
#cores div{flex:1;min-width:3px;border-radius:2px}
canvas{width:100%;height:90px;display:block}
.s{grid-column:1/-1}
</style></head><body><div class=w><h2 id=h>PC Monitor</h2><div class=g>
<div class=c><div class=l>CPU</div><div class=big id=cpu>--</div><div class=l id=sub></div><div class=l id=st>connecting</div></div>
<div class=c><div class=l>CORES</div><div id=cores></div></div>
<div class="c s"><div class=l>CPU HISTORY (2 min)</div><canvas id=cv></canvas></div>
<div class=c><div class=l>RAM <span id=rt></span></div><div class=bar><i id=rb></i></div><div class=l>DISK <span id=dt></span></div><div class=bar><i id=db></i></div></div>
<div class=c><div class=l>NETWORK</div><div id=net>--</div><div class=l style="margin-top:12px" id=upt></div></div>
</div></div><script>
const $=i=>document.getElementById(i);
const col=p=>p<60?'#3fb950':p<85?'#d29922':'#f85149';
const rate=k=>k>=1024?(k/1024).toFixed(1)+' MB/s':Math.round(k)+' KB/s';
const upt=s=>{const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'d ':'')+h+'h '+m+'m'};
const H=[];
function bar(id,p){const e=$(id);e.style.width=p+'%';e.style.background=col(p)}
function draw(){const c=$('cv'),x=c.getContext('2d');c.width=c.clientWidth;c.height=c.clientHeight;x.clearRect(0,0,c.width,c.height);const w=c.width/120;H.forEach((v,i)=>{x.fillStyle=col(v);const h=v/100*c.height;x.fillRect(i*w,c.height-h,w-1,h)})}
async function tick(){try{const s=await(await fetch('/stats')).json();
$('h').textContent=s.host+' | '+s.name;
$('cpu').textContent=Math.round(s.cpu)+'%';$('cpu').style.color=col(s.cpu);
$('sub').textContent=s.freq+' MHz | '+(s.temp>0?s.temp+' C':'temp n/a');
$('cores').innerHTML=s.cores.map(c=>'<div style="height:'+Math.max(c,2)+'%;background:'+col(c)+'"></div>').join('');
bar('rb',s.ram);$('rt').textContent=s.ram+'% | '+s.ram_used+'/'+s.ram_total+' GB';
bar('db',s.disk);$('dt').textContent=s.disk+'%';
$('net').textContent='UP '+rate(s.up)+'   DOWN '+rate(s.down);
$('upt').textContent='Uptime '+upt(s.uptime);
H.push(s.cpu);if(H.length>120)H.shift();draw();$('st').textContent='live'}catch(e){$('st').textContent='offline'}}
setInterval(tick,1000);tick();
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body = DASH_HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith("/stats"):
            with lock:
                body = json.dumps(state).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *args):
        pass


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


if __name__ == "__main__":
    threading.Thread(target=sampler, daemon=True).start()
    threading.Thread(target=slow_sampler, daemon=True).start()
    time.sleep(1.2)
    print(f"Agent running. ESP32 config-e PC_IP = {local_ip()}  port = {PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
