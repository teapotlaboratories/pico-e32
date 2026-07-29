#!/usr/bin/env python3
"""Get/set V4L2 camera controls via raw ioctls — a tiny stand-in for `v4l2-ctl` (no v4l-utils needed).
Used by tools/usb_cam_setup.sh to lock the bench USB camera's exposure. See docs/hardware/pico-e32-bench-camera.md.

  tools/v4l2ctl.py list
  tools/v4l2ctl.py set <control_id_hex_or_name_substr> <value>
  tools/v4l2ctl.py --device /dev/video2 list

Default device: the first /dev/video* that offers MJPEG capture (override with --device or CAM_DEVICE).
"""
import fcntl, ctypes, sys, errno, os, glob

def _iowr(t, nr, sz):
    return ((2 | 1) << 30) | (ord(t) << 8) | nr | (sz << 16)

class control(ctypes.Structure):
    _fields_ = [('id', ctypes.c_uint32), ('value', ctypes.c_int32)]

class queryctrl(ctypes.Structure):
    _fields_ = [('id', ctypes.c_uint32), ('type', ctypes.c_uint32), ('name', ctypes.c_char * 32),
                ('minimum', ctypes.c_int32), ('maximum', ctypes.c_int32), ('step', ctypes.c_int32),
                ('default_value', ctypes.c_int32), ('flags', ctypes.c_uint32), ('reserved', ctypes.c_uint32 * 2)]

class fmtdesc(ctypes.Structure):
    _fields_ = [('index', ctypes.c_uint32), ('type', ctypes.c_uint32), ('flags', ctypes.c_uint32),
                ('description', ctypes.c_char * 32), ('pixelformat', ctypes.c_uint32), ('reserved', ctypes.c_uint32 * 4)]

VIDIOC_QUERYCTRL = _iowr('V', 36, ctypes.sizeof(queryctrl))
VIDIOC_G_CTRL = _iowr('V', 27, ctypes.sizeof(control))
VIDIOC_S_CTRL = _iowr('V', 28, ctypes.sizeof(control))
VIDIOC_ENUM_FMT = _iowr('V', 2, ctypes.sizeof(fmtdesc))
NEXT, DISABLED, MJPEG = 0x80000000, 0x0001, 0x47504A4D
TYPES = {1: 'int', 2: 'bool', 3: 'menu', 4: 'button', 5: 'int64', 6: 'class', 9: 'intmenu'}

def detect_device():
    for dev in sorted(glob.glob('/dev/video*')):
        try:
            fd = os.open(dev, os.O_RDWR)
        except OSError:
            continue
        try:
            i = 0
            while i <= 32:
                f = fmtdesc(); f.index = i; f.type = 1
                try:
                    fcntl.ioctl(fd, VIDIOC_ENUM_FMT, f)
                except OSError:
                    break
                if f.pixelformat == MJPEG:
                    return dev
                i += 1
        finally:
            os.close(fd)
    return None

def enumerate_ctrls(fd):
    out, qid = [], NEXT
    while True:
        q = queryctrl(); q.id = qid
        try:
            fcntl.ioctl(fd, VIDIOC_QUERYCTRL, q)
        except OSError as e:
            if e.errno == errno.EINVAL:
                break
            raise
        cur = None
        if not (q.flags & DISABLED) and q.type != 6:
            c = control(); c.id = q.id
            try:
                fcntl.ioctl(fd, VIDIOC_G_CTRL, c); cur = c.value
            except OSError:
                cur = None
        out.append((q.id, q.name.decode(errors='replace'), TYPES.get(q.type, q.type),
                    q.minimum, q.maximum, q.step, q.default_value, cur, q.flags))
        qid = q.id | NEXT
    return out

def main():
    args = sys.argv[1:]
    dev = os.environ.get('CAM_DEVICE')
    if '--device' in args:
        i = args.index('--device'); dev = args[i + 1]; del args[i:i + 2]
    dev = dev or detect_device()
    if not dev:
        print('no MJPEG-capable /dev/video* found', file=sys.stderr); sys.exit(1)
    fd = open(dev, 'rb+', buffering=0)
    if args and args[0] == 'set':
        key, val = args[1], int(args[2])
        match = None
        for cid, name, *_ in enumerate_ctrls(fd):
            if key.lower() in ('0x%08x' % cid,) or key.lower() in name.lower():
                match = (cid, name); break
        if not match:
            print('no control matching', key); sys.exit(1)
        c = control(); c.id = match[0]; c.value = val
        fcntl.ioctl(fd, VIDIOC_S_CTRL, c)
        c2 = control(); c2.id = match[0]; fcntl.ioctl(fd, VIDIOC_G_CTRL, c2)
        print('set %s (0x%08x) -> %d (readback %d)' % (match[1], match[0], val, c2.value))
    else:
        print('device: %s' % dev)
        print('%-28s %-8s %8s %8s %6s %8s %8s  %s' % ('name', 'type', 'min', 'max', 'step', 'default', 'current', 'id'))
        for cid, name, typ, mn, mx, st, dv, cur, fl in enumerate_ctrls(fd):
            print('%-28s %-8s %8d %8d %6d %8d %8s  0x%08x%s' %
                  (name, typ, mn, mx, st, dv, ('-' if cur is None else cur), cid, ' [disabled]' if fl & DISABLED else ''))

if __name__ == '__main__':
    main()
