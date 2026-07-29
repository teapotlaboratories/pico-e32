#!/usr/bin/env python3
"""Multi-client MJPEG webserver for a USB (UVC) bench camera — a second bench eye alongside the ESP32
network cam (see docs/hardware/pico-e32-bench-camera.md). One long-lived ffmpeg reads the camera; every
HTTP client gets a live multipart/x-mixed-replace stream of the latest frame, so multiple browsers can
watch at once with no reconnect gaps. Open http://<host>:<port>/ in a browser (or an <img src=...>).

  tools/usb_cam_relay.py                 # auto-detect the camera, 2560x1440 @ 30fps on :8090
  CAM_W=1920 CAM_H=1080 tools/usb_cam_relay.py
  CAM_DEVICE=/dev/video2 CAM_PORT=8091 tools/usb_cam_relay.py

Env: CAM_DEVICE (default: auto-detect the first MJPEG-capable node), CAM_W/CAM_H (default 2560x1440),
CAM_FPS (30), CAM_PORT (8090). Needs ffmpeg. Pair with tools/usb_cam_setup.sh to lock exposure for a panel.
"""
import subprocess, threading, http.server, socketserver, os, sys, fcntl, ctypes, errno, glob

# --- V4L2 fmt enumeration, to auto-find the capture node that offers MJPEG ---------------------------------
def _iowr(t, nr, sz):
    return ((2 | 1) << 30) | (ord(t) << 8) | nr | (sz << 16)

class _fmtdesc(ctypes.Structure):
    _fields_ = [('index', ctypes.c_uint32), ('type', ctypes.c_uint32), ('flags', ctypes.c_uint32),
                ('description', ctypes.c_char * 32), ('pixelformat', ctypes.c_uint32),
                ('reserved', ctypes.c_uint32 * 4)]

_VIDIOC_ENUM_FMT = _iowr('V', 2, ctypes.sizeof(_fmtdesc))
_MJPEG = 0x47504A4D  # 'MJPG'

def _has_mjpeg(dev):
    try:
        fd = os.open(dev, os.O_RDWR)
    except OSError:
        return False
    try:
        i = 0
        while True:
            f = _fmtdesc(); f.index = i; f.type = 1  # VIDEO_CAPTURE
            try:
                fcntl.ioctl(fd, _VIDIOC_ENUM_FMT, f)
            except OSError as e:
                return False if e.errno == errno.EINVAL else False
            if f.pixelformat == _MJPEG:
                return True
            i += 1
            if i > 32:
                return False
    finally:
        os.close(fd)

def detect_device():
    for dev in sorted(glob.glob('/dev/video*')):
        if _has_mjpeg(dev):
            return dev
    return None

DEV = os.environ.get('CAM_DEVICE') or detect_device()
W = int(os.environ.get('CAM_W', 2560))
H = int(os.environ.get('CAM_H', 1440))
FPS = int(os.environ.get('CAM_FPS', 30))
PORT = int(os.environ.get('CAM_PORT', 8090))

if not DEV:
    sys.stderr.write('no MJPEG-capable /dev/video* found (is the camera plugged in?)\n'); sys.exit(1)

_lock = threading.Condition()
_latest = [None]

def reader():
    while True:
        try:
            p = subprocess.Popen(
                ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'v4l2', '-input_format', 'mjpeg',
                 '-video_size', '%dx%d' % (W, H), '-framerate', str(FPS), '-i', DEV,
                 '-c:v', 'copy', '-f', 'mjpeg', 'pipe:1'],
                stdout=subprocess.PIPE, bufsize=0)
        except FileNotFoundError:
            sys.stderr.write('ffmpeg not found on PATH\n'); sys.stderr.flush(); os._exit(2)
        buf = b''
        while True:
            chunk = p.stdout.read(1 << 16)
            if not chunk:
                break
            buf += chunk
            while True:
                s = buf.find(b'\xff\xd8')
                e = buf.find(b'\xff\xd9', s + 2) if s >= 0 else -1
                if s < 0 or e < 0:
                    break
                with _lock:
                    _latest[0] = buf[s:e + 2]
                    _lock.notify_all()
                buf = buf[e + 2:]
        try:
            p.kill()
        except Exception:
            pass

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
        self.send_header('Cache-Control', 'no-cache, private')
        self.send_header('Connection', 'close')
        self.end_headers()
        last = None
        try:
            while True:
                with _lock:
                    _lock.wait(timeout=5)
                    f = _latest[0]
                if f is None or f is last:
                    continue
                last = f
                self.wfile.write(b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n' % len(f))
                self.wfile.write(f)
                self.wfile.write(b'\r\n')
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *a):
        pass

class Srv(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

if __name__ == '__main__':
    threading.Thread(target=reader, daemon=True).start()
    print('usb_cam_relay: %dx%d@%d from %s -> http://0.0.0.0:%d/' % (W, H, FPS, DEV, PORT), flush=True)
    Srv(('0.0.0.0', PORT), Handler).serve_forever()
