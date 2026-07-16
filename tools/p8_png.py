#!/usr/bin/env python3
"""Render a PICO-8 indexed buffer (one palette index per byte) to a PNG.

This is the camera-independent way to *look at* what the runtime drew: the host build of
pico-e32-host (-DHOST_MAIN) dumps raw indexed frames, and this turns them into something a
human (or a reviewer) can actually inspect — see docs/runtime/pico-e32-host-graphics.md.
It proves the framebuffer is right; it says nothing about the physical panel (that needs a
bench-camera capture — docs/hardware/pico-e32-bench-camera.md).

Usage:
    p8_png.py <in.raw> <out.png> [--w 128] [--h 128] [--scale 4]

Stdlib only (zlib) — no Pillow, so it runs anywhere the repo does.
"""
import sys, zlib, struct

# PICO-8 palette, RGB888. Must stay identical to PAL888 in
# firmware/pico-e32-host/main/host_main.cpp — the whole point is comparing against that.
PAL = [
    (0, 0, 0), (29, 43, 83), (126, 37, 83), (0, 135, 81),
    (171, 82, 54), (95, 87, 79), (194, 195, 199), (255, 241, 232),
    (255, 0, 77), (255, 163, 0), (255, 236, 39), (0, 228, 54),
    (41, 173, 255), (131, 118, 156), (255, 119, 168), (255, 204, 170),
]


def png_bytes(rgb_rows, w, h):
    """Minimal RGB8 PNG encoder (stdlib zlib; no external deps)."""
    raw = b"".join(b"\x00" + row for row in rgb_rows)   # filter 0 per scanline

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def render(data, w, h, scale):
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            v = data[y * w + x]
            if v > 15:
                sys.exit(f"error: palette index {v} at ({x},{y}) is out of range 0-15 — "
                         f"is this really an indexed buffer?")
            row += bytes(PAL[v]) * scale
        rows.extend([bytes(row)] * scale)
    return rows


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = {a.split("=")[0].lstrip("-"): a.split("=")[1]
            for a in argv[1:] if a.startswith("--") and "=" in a}
    if len(args) != 2:
        sys.exit(__doc__)
    src, dst = args
    w = int(opts.get("w", 128)); h = int(opts.get("h", 128)); scale = int(opts.get("scale", 4))

    data = open(src, "rb").read()
    if len(data) != w * h:
        sys.exit(f"error: {src} is {len(data)} B but {w}x{h} needs {w*h} B "
                 f"(one byte per pixel). Pass --w/--h if the buffer isn't 128x128.")

    open(dst, "wb").write(png_bytes(render(data, w, h, scale), w * scale, h * scale))
    print(f"{dst}  ({w}x{h} -> {w*scale}x{h*scale}, scale {scale}x)")


if __name__ == "__main__":
    main(sys.argv)
