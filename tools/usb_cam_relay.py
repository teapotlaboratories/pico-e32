#!/usr/bin/env python3
"""USB (UVC) bench-camera web app — live MJPEG stream + camera controls in one sleek page.

A second bench eye alongside the ESP32 network cam (see docs/hardware/pico-e32-bench-camera.md), driven
straight off /dev/video* via ffmpeg — no firmware. Open http://<host>:<port>/ for the UI (live video +
exposure / brightness / white-balance / gain / … sliders). Raw MJPEG is at /stream (embeddable).

  tools/usb_cam_relay.py                 # auto-detect the camera, 2560x1440 @ 30fps on :8090
  CAM_W=1920 CAM_H=1080 CAM_PORT=8091 tools/usb_cam_relay.py
  CAM_DEVICE=/dev/video2 tools/usb_cam_relay.py

Env: CAM_DEVICE (default: auto-detect the first MJPEG-capable node), CAM_W/CAM_H (default 2560x1440),
CAM_FPS (30), CAM_PORT (8090). Needs ffmpeg. Controls are the camera's own V4L2 controls (reset to auto on
each replug). Endpoints: GET / (UI), GET /stream (MJPEG), GET /api/controls (JSON), GET /api/set?id&value.
"""
import subprocess, threading, http.server, socketserver, os, sys, json, fcntl, ctypes, glob
from urllib.parse import urlparse, parse_qs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import v4l2ctl as v  # shared V4L2 ioctl helpers (enumerate_ctrls, control, detect_device, VIDIOC_*)

class _querymenu(ctypes.Structure):
    _fields_ = [('id', ctypes.c_uint32), ('index', ctypes.c_uint32), ('name', ctypes.c_char * 32), ('r', ctypes.c_uint32)]
VIDIOC_QUERYMENU = v._iowr('V', 37, ctypes.sizeof(_querymenu))

DEV = os.environ.get('CAM_DEVICE') or v.detect_device()
W = int(os.environ.get('CAM_W', 2560)); H = int(os.environ.get('CAM_H', 1440))
FPS = int(os.environ.get('CAM_FPS', 30)); PORT = int(os.environ.get('CAM_PORT', 8090))
if not DEV:
    sys.stderr.write('no MJPEG-capable /dev/video* found (is the camera plugged in?)\n'); sys.exit(1)

_lock = threading.Condition(); _latest = [None]

def reader():
    while True:
        try:
            p = subprocess.Popen(
                ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'v4l2', '-input_format', 'mjpeg',
                 '-video_size', '%dx%d' % (W, H), '-framerate', str(FPS), '-i', DEV,
                 '-c:v', 'copy', '-f', 'mjpeg', 'pipe:1'], stdout=subprocess.PIPE, bufsize=0)
        except FileNotFoundError:
            sys.stderr.write('ffmpeg not found on PATH\n'); os._exit(2)
        buf = b''
        while True:
            chunk = p.stdout.read(1 << 16)
            if not chunk:
                break
            buf += chunk
            while True:
                s = buf.find(b'\xff\xd8'); e = buf.find(b'\xff\xd9', s + 2) if s >= 0 else -1
                if s < 0 or e < 0:
                    break
                with _lock:
                    _latest[0] = buf[s:e + 2]; _lock.notify_all()
                buf = buf[e + 2:]
        try:
            p.kill()
        except Exception:
            pass

# --- camera controls (a fresh fd per op works fine alongside the streaming ffmpeg) ------------------------
def _menu(fd, cid, mn, mx):
    items = []
    for i in range(mn, mx + 1):
        m = _querymenu(); m.id = cid; m.index = i
        try:
            fcntl.ioctl(fd, VIDIOC_QUERYMENU, m); items.append({'value': i, 'label': m.name.decode(errors='replace')})
        except OSError:
            pass
    return items

def list_controls():
    fd = os.open(DEV, os.O_RDWR)
    try:
        groups, cur_group = [], None
        for cid, name, typ, mn, mx, st, dv, cur, fl in v.enumerate_ctrls(fd):
            if typ == 'class':
                cur_group = {'name': name, 'controls': []}; groups.append(cur_group); continue
            if cur_group is None:
                cur_group = {'name': 'Controls', 'controls': []}; groups.append(cur_group)
            c = {'id': cid, 'name': name, 'type': typ, 'min': mn, 'max': mx, 'step': st,
                 'default': dv, 'value': cur, 'disabled': bool(fl & 0x0001)}
            if typ == 'menu':
                c['menu'] = _menu(fd, cid, mn, mx)
            cur_group['controls'].append(c)
        return groups
    finally:
        os.close(fd)

def set_control(cid, val):
    fd = os.open(DEV, os.O_RDWR)
    try:
        c = v.control(); c.id = cid; c.value = val
        fcntl.ioctl(fd, v.VIDIOC_S_CTRL, c)
        c2 = v.control(); c2.id = cid; fcntl.ioctl(fd, v.VIDIOC_G_CTRL, c2)
        return c2.value
    finally:
        os.close(fd)

PAGE = ("""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Bench Camera</title>
<style>
 :root{--bg:#0c0f14;--panel:#151a22;--panel2:#1c232e;--bd:#2a3340;--fg:#e6edf3;--mut:#8b97a6;
   --acc:#4c8dff;--acc2:#3fb6a8;--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
 *{box-sizing:border-box}
 body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif}
 header{display:flex;align-items:center;gap:.7rem;padding:.8rem 1.1rem;border-bottom:1px solid var(--bd);background:var(--panel)}
 header h1{font-size:1.02rem;margin:0;letter-spacing:-.01em}
 header .dot{width:9px;height:9px;border-radius:50%;background:var(--acc2);box-shadow:0 0 8px var(--acc2)}
 header .meta{margin-left:auto;color:var(--mut);font:12px/1 var(--mono)}
 .wrap{display:flex;gap:1rem;padding:1rem;align-items:flex-start;flex-wrap:wrap}
 .stage{flex:1 1 520px;min-width:300px;background:#000;border:1px solid var(--bd);border-radius:14px;overflow:hidden;position:relative}
 .stage img{display:block;width:100%;height:auto}
 .panel{flex:0 0 340px;max-width:100%;background:var(--panel);border:1px solid var(--bd);border-radius:14px;padding:.4rem .2rem .8rem;max-height:82vh;overflow:auto}
 .grp{padding:.5rem .9rem}
 .grp h2{font:600 .72rem/1 var(--mono);text-transform:uppercase;letter-spacing:.09em;color:var(--mut);margin:.7rem 0 .3rem}
 .ctl{padding:.5rem .1rem;border-top:1px solid var(--bd)}
 .ctl:first-of-type{border-top:0}
 .ctl .row{display:flex;align-items:center;gap:.6rem;margin-bottom:.35rem}
 .ctl label{font-size:.86rem;flex:1;color:var(--fg)}
 .ctl .val{font:600 .78rem/1 var(--mono);color:var(--acc);min-width:3ch;text-align:right}
 input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:5px;border-radius:5px;
   background:linear-gradient(90deg,var(--acc) var(--pct,50%),var(--panel2) var(--pct,50%));outline:none}
 input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:16px;height:16px;border-radius:50%;background:#fff;border:2px solid var(--acc);cursor:pointer;transition:transform .1s}
 input[type=range]::-webkit-slider-thumb:active{transform:scale(1.2)}
 input[type=range]::-moz-range-thumb{width:14px;height:14px;border-radius:50%;background:#fff;border:2px solid var(--acc);cursor:pointer}
 input[type=range]:disabled{opacity:.4}
 select{width:100%;background:var(--panel2);color:var(--fg);border:1px solid var(--bd);border-radius:8px;padding:.4rem .5rem;font-size:.85rem}
 .sw{position:relative;width:40px;height:22px;flex:0 0 auto}
 .sw input{opacity:0;width:0;height:0}
 .sw span{position:absolute;inset:0;background:var(--panel2);border:1px solid var(--bd);border-radius:22px;cursor:pointer;transition:.15s}
 .sw span:before{content:"";position:absolute;height:16px;width:16px;left:2px;top:2px;background:#fff;border-radius:50%;transition:.15s}
 .sw input:checked+span{background:var(--acc);border-color:var(--acc)}
 .sw input:checked+span:before{transform:translateX(18px)}
 .bar{display:flex;gap:.5rem;padding:.7rem .9rem .2rem}
 button{flex:1;background:var(--panel2);color:var(--fg);border:1px solid var(--bd);border-radius:9px;padding:.55rem;font-size:.83rem;cursor:pointer;transition:.12s}
 button:hover{border-color:var(--acc);color:#fff}
 button.primary{background:var(--acc);border-color:var(--acc);color:#fff}
 button.primary:hover{filter:brightness(1.08)}
 .hint{color:var(--mut);font-size:.72rem;padding:.2rem .9rem 0;line-height:1.4}
</style></head><body>
<header><span class="dot"></span><h1>USB Bench Camera</h1><span class="meta" id="meta"></span></header>
<div class="wrap">
  <div class="stage"><img id="v" src="/stream" alt="live"></div>
  <div class="panel">
    <div class="bar">
      <button class="primary" id="preset">Lock exposure (panel)</button>
      <button id="reset">Reset defaults</button>
    </div>
    <div class="hint">Controls are the camera's own; they reset to auto when you unplug it.</div>
    <div id="ctls"></div>
  </div>
</div>
<script>
const $=s=>document.querySelector(s);
async function api(u){const r=await fetch(u);return r.json()}
function setPct(el){const p=(el.value-el.min)/(el.max-el.min)*100;el.style.setProperty('--pct',p+'%')}
let CTRLS=[];
function widget(c){
  const wrap=document.createElement('div');wrap.className='ctl';
  const row=document.createElement('div');row.className='row';
  const lab=document.createElement('label');lab.textContent=c.name;row.appendChild(lab);
  if(c.type==='bool'){
    const sw=document.createElement('label');sw.className='sw';
    const inp=document.createElement('input');inp.type='checkbox';inp.checked=c.value==1;
    const sp=document.createElement('span');sw.appendChild(inp);sw.appendChild(sp);row.appendChild(sw);
    inp.onchange=()=>set(c.id,inp.checked?1:0);
    wrap.appendChild(row);
  }else if(c.type==='menu'){
    wrap.appendChild(row);
    const sel=document.createElement('select');
    (c.menu||[]).forEach(m=>{const o=document.createElement('option');o.value=m.value;o.textContent=m.label||m.value;if(m.value==c.value)o.selected=true;sel.appendChild(o)});
    sel.onchange=()=>set(c.id,parseInt(sel.value));
    wrap.appendChild(sel);
  }else{
    const val=document.createElement('span');val.className='val';val.textContent=c.value;row.appendChild(val);
    wrap.appendChild(row);
    const rng=document.createElement('input');rng.type='range';rng.min=c.min;rng.max=c.max;rng.step=c.step||1;rng.value=c.value;rng.disabled=c.disabled;
    setPct(rng);
    let t;rng.oninput=()=>{val.textContent=rng.value;setPct(rng);clearTimeout(t);t=setTimeout(()=>set(c.id,parseInt(rng.value)),120)};
    wrap.appendChild(rng);
  }
  return wrap;
}
async function set(id,value){await api('/api/set?id='+id+'&value='+value);}
async function load(){
  const d=await api('/api/controls');CTRLS=d.groups;
  $('#meta').textContent=d.device+'  '+d.width+'×'+d.height+'@'+d.fps;
  const root=$('#ctls');root.innerHTML='';
  d.groups.forEach(g=>{
    if(!g.controls.length)return;
    const gd=document.createElement('div');gd.className='grp';
    const h=document.createElement('h2');h.textContent=g.name;gd.appendChild(h);
    g.controls.forEach(c=>gd.appendChild(widget(c)));
    root.appendChild(gd);
  });
}
$('#reset').onclick=async()=>{for(const g of CTRLS)for(const c of g.controls)if(c.type!=='button')await set(c.id,c.default);load()};
$('#preset').onclick=async()=>{
  // manual exposure locked for an emissive panel: dyn-framerate off, manual AE, 6ms, WB manual
  await set(0x009a0903,0);await set(0x009a0901,1);await set(0x009a0902,60);await set(0x0098090c,0);load();
};
load();
</script></body></html>""")

class Handler(http.server.BaseHTTPRequestHandler):
    def _send(self, code, ctype, body):
        self.send_response(code); self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body))); self.send_header('Connection', 'close'); self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        u = urlparse(self.path); path = u.path
        if path == '/' or path == '/index.html':
            self._send(200, 'text/html; charset=utf-8', PAGE.encode())
        elif path == '/api/controls':
            body = json.dumps({'device': DEV, 'width': W, 'height': H, 'fps': FPS, 'groups': list_controls()}).encode()
            self._send(200, 'application/json', body)
        elif path == '/api/set':
            q = parse_qs(u.query)
            try:
                cid = int(q['id'][0], 0); val = int(q['value'][0])
                newv = set_control(cid, val)
                self._send(200, 'application/json', json.dumps({'ok': True, 'value': newv}).encode())
            except Exception as e:
                self._send(400, 'application/json', json.dumps({'ok': False, 'error': str(e)}).encode())
        elif path == '/stream' or path == '/stream.mjpg':
            self.send_response(200)
            self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
            self.send_header('Cache-Control', 'no-cache, private'); self.send_header('Connection', 'close'); self.end_headers()
            last = None
            try:
                while True:
                    with _lock:
                        _lock.wait(timeout=5); f = _latest[0]
                    if f is None or f is last:
                        continue
                    last = f
                    self.wfile.write(b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n' % len(f))
                    self.wfile.write(f); self.wfile.write(b'\r\n')
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self._send(404, 'text/plain', b'not found')

    def log_message(self, *a):
        pass

class Srv(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

if __name__ == '__main__':
    threading.Thread(target=reader, daemon=True).start()
    print('usb_cam_relay: %dx%d@%d from %s -> http://0.0.0.0:%d/  (UI at /, stream at /stream)'
          % (W, H, FPS, DEV, PORT), flush=True)
    Srv(('0.0.0.0', PORT), Handler).serve_forever()
