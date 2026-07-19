#!/usr/bin/env python3
"""Hands-free Celeste play-test: drive the fake-08 port through TWO FULL LEVELS over serial and verify it.

Clears **Celeste rooms 1 and 2** — "100 M" -> "200 M" -> "300 M" — with no human on the controls, and
confirms each clear over the wire. A repeatable, self-checking integration test for the whole fake-08
stack (cart load -> VM -> input -> physics -> the 30/60 fps loop). Exits 0 on PASS, 1 on FAIL.

HOW IT WORKS
    Each room needs several frame-precise moves (dashes/jumps) that a loose open-loop timeline cannot land.
    So the input for each room was solved OFFLINE against a faithful re-implementation of Celeste's player
    physics + that room's map (decoded from the cart), then delivered FRAME-SYNCED: the firmware streams the
    player's position + a frame counter each frame (TELEMETRY build), and this driver sends each frame's
    buttons locked to that counter (INPUT_HOLD_FRAMES=1 = frame-exact). After a room is cleared the player
    respawns at the next room's spawn, so the driver re-syncs and delivers that room's plan. Delivery is
    deterministic -> the same clear at the same frame, every run.

    The driver DRAINS the telemetry each iteration so the frame counter stays real-time (otherwise the
    60 Hz stream backs up and delivery lags on the later room). The solver is `tools/celeste_solver/`;
    the PLANs below are its output, embedded so this test is self-contained.
    See docs/worklog/2026-07-18-celeste-playtest-clear.md.

REQUIRED FIRMWARE (build + flash first, from the repo root):
    make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<board> \\
         DEFS='-D CELESTE=1 -D INPUT_BACKEND=serial -D INPUT_HOLD_FRAMES=1 \\
               -D FORCE_FLASH_CART=1 -D SHOW_FPS=1 -D TELEMETRY=1 -D CENTER_GAME=1'
    (CENTER_GAME=1 vertically centres the game on the panel — the serial build has no touch control-deck,
     so the game needn't sit flush to the top; drop it for the normal top-aligned layout.)

RUN:
    python3 tools/celeste_playtest.py <board-port>
    Ports are NOT stable -- identify the BOARD by USB chip (leaves the camera untouched):
        udevadm info -q property -n /dev/ttyUSBx | grep ID_USB_DRIVER
        board  = CP2104  (ID_USB_DRIVER=cp210x)     <- use this port
        camera = FTDI    (ID_USB_DRIVER=ftdi_sio)

Serial keys (input_serial): l/r/u/d dirs, z (or o) = O = JUMP (k_jump=4), x = X = DASH (k_dash=5).
"""
import serial, sys, time

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
ROOMS = [
    dict(name="100 M", rx=0, ry=0, spawn=(8.0, 96.0), plan=PLAN_100M),
    dict(name="200 M", rx=1, ry=0, spawn=(8.0, 112.0), plan=PLAN_200M),
]

SETTLE = 8      # game-frames to idle at each room's spawn before frame 0
LEAD = 2        # firmware frames to send ahead of the target update (serial + poll latency)
START_SENDS = [(4.0, b'z'), (4.6, b'z'), (5.2, b'z')]   # title -> begin_game (jump; ignored once started)


def nextroom(rx, ry):
    return (0, ry + 1) if rx == 7 else (rx + 1, ry)


def parse_T(line):
    p = line.split()
    if len(p) < 6 or p[0] != b'T':
        return None
    def num(s):
        try: return float(s)
        except Exception: return None
    return dict(fc=int(p[1]), x=num(p[2]), y=num(p[3]), rx=num(p[4]), ry=num(p[5]))


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
    p = serial.Serial(port, 115200, timeout=0.02)
    # reset into a normal boot: RTS=EN pulse low->high, DTR=GPIO0 held high
    p.dtr = False; p.rts = True
    time.sleep(0.15); p.reset_input_buffer(); p.rts = False

    t0 = time.monotonic()
    si = 0
    cur = 0             # index into ROOMS of the room being cleared
    plan_fc0 = None     # frame counter mapped to the current room's plan index 0
    sent = set()
    cleared = []
    done_t = None
    buf = b""
    while True:
        now = time.monotonic() - t0
        if now > 42 or (done_t and now > done_t + 2):
            break
        while si < len(START_SENDS) and START_SENDS[si][0] <= now:
            p.write(START_SENDS[si][1]); p.flush(); si += 1
        # Drain the telemetry each iteration so the frame counter stays real-time (else the 60 Hz stream
        # backs up in the OS buffer and delivery lags on the later room).
        chunk = p.read(p.in_waiting or 1)
        if chunk:
            buf += chunk
        latest = None
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = parse_T(line)
            if t is None:
                continue
            latest = t
            if cur >= len(ROOMS):
                continue
            R = ROOMS[cur]
            rid = (R['rx'], R['ry']); nid = nextroom(*rid)
            if t['x'] is not None and (t['rx'], t['ry']) == (float(nid[0]), float(nid[1])):
                cleared.append(cur)
                nm = R['name']; nxt = (1 + nid[0] % 8 + nid[1] * 8) * 100
                print(f"CLEARED {nm} -> room ({nid[0]},{nid[1]}) = {nxt} M  at t={now:.2f}s")
                cur += 1; plan_fc0 = None; sent = set()
                if cur >= len(ROOMS):
                    done_t = now
                continue
            if plan_fc0 is None and t['x'] is not None and (t['rx'], t['ry']) == (float(rid[0]), float(rid[1])) \
                    and (t['x'], t['y']) == R['spawn']:
                plan_fc0 = t['fc'] + 2 * SETTLE
        # deliver the current room's plan against the latest (real-time) frame counter
        if latest is not None and cur < len(ROOMS) and plan_fc0 is not None:
            plan = ROOMS[cur]['plan']; fc = latest['fc']
            for f in range(len(plan)):
                if f in sent or not plan[f]:
                    continue
                if fc >= plan_fc0 + 2 * f - LEAD:
                    for c in plan[f]:
                        p.write(c.encode())
                    sent.add(f)
            p.flush()
    p.close()

    if len(cleared) >= len(ROOMS):
        print(f"PASS: Celeste levels 1-2 cleared hands-free over serial ({len(ROOMS)}/{len(ROOMS)} rooms).")
        return 0
    print(f"FAIL: cleared {len(cleared)}/{len(ROOMS)} rooms. Check the build flags (TELEMETRY=1, "
          "INPUT_HOLD_FRAMES=1, INPUT_BACKEND=serial, CELESTE=1) and that <port> is the board (CP2104).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
