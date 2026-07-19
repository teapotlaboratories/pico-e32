#!/usr/bin/env python3
"""Hands-free Celeste play-test: drive the fake-08 port through TWO FULL LEVELS over serial and verify it.

Clears **Celeste rooms 1 and 2** — "100 M" -> "200 M" -> "300 M" — with no human on the controls, and
confirms each clear over the wire. A repeatable, self-checking integration test for the whole fake-08
stack (cart load -> VM -> input -> physics -> the 30/60 fps loop). Exits 0 on PASS, 1 on FAIL.

This is the reference user of the generic harness (`../harness.py`): it supplies only the Celeste-specific
bits — the telemetry parser, the per-room solved plans, and each room as a harness `Segment` (start = at
the room's spawn; clear = advanced to the next room). The frame-synced delivery engine is the harness.

HOW IT WORKS
    Each room needs several frame-precise moves (dashes/jumps) that a loose open-loop timeline cannot land.
    So the input for each room was solved OFFLINE against a faithful re-implementation of Celeste's player
    physics + that room's map (decoded from the cart), then delivered FRAME-SYNCED: the firmware streams the
    player's position + a frame counter each frame (TELEMETRY build), and the harness sends each frame's
    buttons locked to that counter (INPUT_HOLD_FRAMES=1 = frame-exact). After a room is cleared the player
    respawns at the next room's spawn, so the harness re-syncs and delivers that room's plan. Delivery is
    deterministic -> the same clear at the same frame, every run. The solver is `celeste_solver/`; the PLANs
    below are its output, embedded so this test is self-contained.
    See docs/worklog/2026-07-18-celeste-playtest-clear.md.

REQUIRED FIRMWARE (build + flash first, from the repo root):
    make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<board> \\
         DEFS='-D CELESTE=1 -D INPUT_BACKEND=serial -D INPUT_HOLD_FRAMES=1 \\
               -D FORCE_FLASH_CART=1 -D SHOW_FPS=1 -D TELEMETRY=1 -D CENTER_GAME=1'
    (CENTER_GAME=1 vertically centres the game on the panel — the serial build has no touch control-deck,
     so the game needn't sit flush to the top; drop it for the normal top-aligned layout.)

RUN:
    python3 test/playtest/celeste/celeste_playtest.py <board-port>
    Ports are NOT stable -- identify the BOARD by USB chip (leaves the camera untouched):
        udevadm info -q property -n /dev/ttyUSBx | grep ID_USB_DRIVER
        board  = CP2104  (ID_USB_DRIVER=cp210x)     <- use this port
        camera = FTDI    (ID_USB_DRIVER=ftdi_sio)

Serial keys (input_serial): l/r/u/d dirs, z (or o) = O = JUMP (k_jump=4), x = X = DASH (k_dash=5).
"""
import sys, os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))  # find harness.py
from harness import Segment, run

# --- Per-room solved plans: one entry per GAME-frame (30 fps), the keys held that frame. Freeze frames
#     (2 skipped frames after each dash) are baked in, so index == the firmware frame counter / 2. -------
PLAN_100M = [  # room (0,0): dash-UR, run, dash-UR, run, jump, run-left, dash-U, run, jump, run, jump, dash-U
    'rux', 'rux', 'rux', 'ru', 'ru', 'ru', 'ru', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r',
    'r', 'r', 'r', 'r', 'rux', 'rux', 'rux', 'ru', 'ru', 'ru', 'ru', 'r', 'r', 'r', 'r', 'r', 'r',
    'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'z', '', '', '', 'l', 'l', 'l', 'l', 'l', 'l', 'l', 'l',
    'l', 'l', 'ux', 'ux', 'ux', 'u', 'u', 'u', 'u', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r',
    'r', 'r', 'r', 'r', 'z', '', '', '', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'z', '', '', '',
    'ux', 'ux', 'ux', 'u',
]
PLAN_200M = [  # room (1,0): run + a stack of jumps, one dash-UR near the top, more jumps to the exit
    'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'z', '', '', '', 'r', 'r',
    'r', 'r', 'r', 'r', 'r', 'r', 'z', '', '', '', 'z', '', '', '', 'z', '', '', '', 'r', 'r', 'r',
    'r', 'z', '', '', '', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'rux',
    'rux', 'rux', 'ru', 'ru', 'ru', 'ru', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r',
    'r', 'r', 'z', '', '', '', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r', 'r',
    'z', '', '', '', 'z', '', '', '', 'r', 'r', 'r', 'r', 'rz', 'r',
]

START_SENDS = [(4.0, b'z'), (4.6, b'z'), (5.2, b'z')]   # title -> begin_game (jump; ignored once started)


def parse_line(line):
    """Celeste TELEMETRY line: `T <frame> <x> <y> <room.x> <room.y> <spd.x> <spd.y> <djump>`."""
    p = line.split()
    if len(p) < 6 or p[0] != b'T':
        return None
    def num(s):
        try: return float(s)
        except Exception: return None
    return dict(fc=int(p[1]), x=num(p[2]), y=num(p[3]), rx=num(p[4]), ry=num(p[5]))


def _nextroom(rx, ry):
    return (0, ry + 1) if rx == 7 else (rx + 1, ry)   # rooms are an 8-wide grid


def _room(name, rx, ry, spawn, plan):
    """A Celeste room as a harness Segment: start = player at this room's spawn; clear = the player has
    advanced to the next room in the grid."""
    nid = _nextroom(rx, ry)
    def at_start(st):
        return st['x'] is not None and (st['rx'], st['ry']) == (float(rx), float(ry)) \
            and (st['x'], st['y']) == spawn
    def cleared(st):
        return st['x'] is not None and (st['rx'], st['ry']) == (float(nid[0]), float(nid[1]))
    def describe(st):
        nxt = (1 + nid[0] % 8 + nid[1] * 8) * 100
        return f"{name} -> room ({nid[0]},{nid[1]}) = {nxt} M"
    return Segment(name, plan, at_start, cleared, describe)


SEGMENTS = [
    _room("100 M", 0, 0, (8.0, 96.0), PLAN_100M),
    _room("200 M", 1, 0, (8.0, 112.0), PLAN_200M),
]


def segments_from_trace(path):
    """Build the harness segments from a portable trace file (test/playtest/trace.py) instead of the
    embedded plans — so a solution solved on the sim replays on the device from the *same* artifact.
    Each Celeste segment's meta carries its room + spawn for start/clear detection."""
    from trace import Trace, mask_to_keys
    tr = Trace.load(path)
    segs = []
    for s in tr.segments:
        rx, ry = int(s.meta["rx"]), int(s.meta["ry"])
        spawn = tuple(s.meta["spawn"])
        segs.append(_room(s.name, rx, ry, spawn, [mask_to_keys(m) for m in s.frames]))
    return segs, tr.steps_per_frame


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = args[0] if args else "/dev/ttyUSB0"
    segments, spf = SEGMENTS, 2
    for a in sys.argv[1:]:                     # optional: --trace <file> replays a solved trace
        if a.startswith("--trace="):
            segments, spf = segments_from_trace(a.split("=", 1)[1])
    n = run(port, parse_line, segments, settle=8, lead=2, steps_per_frame=spf,
            start_sends=START_SENDS, timeout=42, done_grace=2)
    total = len(segments)
    if n >= total:
        print(f"PASS: Celeste levels 1-2 cleared hands-free over serial ({n}/{total} rooms).")
        return 0
    print(f"FAIL: cleared {n}/{total} rooms. Check the build flags (TELEMETRY=1, "
          "INPUT_HOLD_FRAMES=1, INPUT_BACKEND=serial, CELESTE=1) and that <port> is the board (CP2104).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
