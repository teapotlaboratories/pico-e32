#!/usr/bin/env python3
"""fb_screenshot.py — camera-free "screenshot" of the device panel via the FB_DUMP firmware.

Pairs with an FB_DUMP build (firmware/pico-e32-fake08, DEFS='-D FB_DUMP=1', ESP32-P4): the board streams
its live DPI framebuffer (RGB565) once over the console, framed by a magic header. This reads it and writes
a PPM (P6). Convert to PNG with ImageMagick:  convert shot.ppm shot.png

  tools/fb_screenshot.py <serial-port> <out.ppm>

Resets the board so the one-shot dump (fired ~2 s after boot) is caught fresh.
"""
import sys, time, serial

MAGIC = b'\xFB\xFB\xFB\xFBSHOT'   # 8 bytes, then w(2) h(2) little-endian, then w*h RGB565 LE

def main():
    if len(sys.argv) != 3:
        print("usage: fb_screenshot.py <port> <out.ppm>"); return 1
    port, out = sys.argv[1], sys.argv[2]
    s = serial.Serial(port, 115200, timeout=1)
    s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)   # reset -> fresh boot + dump

    buf = bytearray()
    t_end = time.time() + 25
    # 1) find the magic header
    while MAGIC not in buf:
        buf += s.read(8192)
        if time.time() > t_end:
            print("timeout: no FB_DUMP header seen"); return 1
    idx = buf.index(MAGIC) + len(MAGIC)
    while len(buf) < idx + 4:
        buf += s.read(64)
    w = buf[idx] | (buf[idx + 1] << 8)
    h = buf[idx + 2] | (buf[idx + 3] << 8)
    idx += 4
    need = w * h * 2
    print(f"header: {w}x{h}  ({need} bytes)")
    # 2) read the raw framebuffer
    t_end = time.time() + 40
    while len(buf) - idx < need:
        chunk = s.read(65536)
        buf += chunk
        if not chunk and time.time() > t_end:
            print(f"timeout: got {len(buf)-idx}/{need} bytes"); return 1
    s.close()
    data = buf[idx:idx + need]
    # 3) RGB565 LE -> RGB888 -> PPM
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        v = data[2 * i] | (data[2 * i + 1] << 8)
        r = (v >> 11) & 0x1f; g = (v >> 5) & 0x3f; b = v & 0x1f
        rgb[3 * i]     = (r << 3) | (r >> 2)
        rgb[3 * i + 1] = (g << 2) | (g >> 4)
        rgb[3 * i + 2] = (b << 3) | (b >> 2)
    with open(out, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(rgb)
    print(f"wrote {out} ({w}x{h})")
    return 0

if __name__ == "__main__":
    sys.exit(main())
