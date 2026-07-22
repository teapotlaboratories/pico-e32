#!/usr/bin/env python3
"""Drive Celeste room (0,0) on the BOARD via the fc-scheduled input backend (frame-exact over telemetry).

The closed-loop policy is solved on the host twin (closed_loop.py — clears the sim in 90 frames); its per-frame
masks are delivered to the device as fc-TAGGED commands (fc_sched.encode_cmd), each targeting the exact frame
its action must land on. The device (INPUT_BACKEND=scheduled) applies each on its target frame regardless of
when the byte arrived, so the ~1-frame serial JITTER that kills a raw-key delivery is removed. This is the
hardware test of the fc-scheduled design (docs/runtime/pico-e32-fake08-input.md IN-5).

REQUIRED FIRMWARE (build + flash from the repo root):
    make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<board> \\
      DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1 -D INPUT_BACKEND=scheduled \\
            -D TELEMETRY=1 -D SHOW_FPS=1 -D CENTER_GAME=1'
The driver skips the title itself, over the same fc-scheduled backend it uses to play: a short warmup pulses
jump commands to press "start", so the player spawns at (8,96) with no raw-key press and no cart-specific
autostart (a boot-time CELESTE_START begin_game()/load_room does not survive the title's deferred init, so the
title-flow start supersedes it). Telemetry stays the plain Celeste ASCII tail — telemetry jitter is irrelevant
here (it is not in the control loop; the fc-commands are).

RUN:  python3 test/playtest/celeste/fc_device.py <board-port> [--k N]   (board = CP2104, e.g. /dev/ttyUSB0)
"""
import sys, os, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))                 # fc_sched, harness
sys.path.insert(0, os.path.join(HERE, "..", "fake08-sim"))   # fake08sim (host twin)
sys.path.insert(0, HERE)                                     # closed_loop, celeste_playtest
import fake08sim as VM                                       # the sim VM = the host twin
from fc_sched import encode_cmd
import closed_loop as CL
from celeste_playtest import parse_line                      # the plain Celeste ASCII telemetry parser

STEP = 2                # firmware fc per game-frame (30 fps cart, 60 Hz resume)
SPAWN = (8.0, 96.0)


def _solve_masks():
    """The host twin's clearing mask sequence for room (0,0) (one int mask per game-frame)."""
    r = CL.drive_sim(record=True, verbose=False)
    if not r["cleared"]:
        raise RuntimeError("the sim policy did not clear room (0,0) — fix closed_loop.py before device delivery")
    return r["masks"]


def drive_device(port, k=2, settle=8, timeout=30.0, verbose=True):
    """Deliver the room-(0,0) masks as fc-tagged commands with `k` game-frames of lead; return (cleared, info)."""
    import serial
    masks = _solve_masks()
    p = serial.Serial(port, 115200, timeout=0.02)
    # reset into a normal boot (RTS=EN pulse, DTR=GPIO0 high) so the run starts clean
    p.dtr = False; p.rts = True
    time.sleep(0.15); p.reset_input_buffer(); p.rts = False

    t0 = time.monotonic(); buf = b""
    plan_fc0 = None; spawn_fc = None; sent = set(); latest = None; wu_last = -999
    cleared = False; clear_fc = None; final = None
    while time.monotonic() - t0 < timeout:
        chunk = p.read(p.in_waiting or 1)
        if chunk:
            buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            st = parse_line(line)
            if st is None:
                continue
            latest = st
            if st["x"] is not None:
                final = st
            # title-skip: the fc-scheduled backend takes no raw keys, so press "start" with jump commands
            # until the game leaves the title, then let the spawn animation land (replaces CELESTE_START).
            if plan_fc0 is None and st["x"] is None and (st["rx"], st["ry"]) == (7.0, 3.0) \
                    and st["fc"] - wu_last >= 8:
                p.write(encode_cmd(st["fc"] + 6, 16, 3)); p.flush(); wu_last = st["fc"]
            # anchor: the player is at room (0,0)'s spawn (static until the first fc-command lands)
            if plan_fc0 is None and st["x"] is not None and (st["rx"], st["ry"]) == (0.0, 0.0) \
                    and abs(st["x"] - SPAWN[0]) < 0.5 and abs(st["y"] - SPAWN[1]) < 0.5:
                spawn_fc = st["fc"]; plan_fc0 = spawn_fc + STEP * settle
            # clear: advanced out of room (0,0)
            if plan_fc0 is not None and st["x"] is not None and (st["rx"], st["ry"]) not in ((0.0, 0.0),):
                cleared = True; clear_fc = st["fc"]
        if cleared:
            break
        # deliver each plan frame's command k game-frames early; the device applies it AT its target fc
        if latest is not None and plan_fc0 is not None and latest["x"] is not None:
            fc = latest["fc"]
            for f in range(len(masks)):
                if f in sent:
                    continue
                target = plan_fc0 + STEP * f
                if fc >= target - STEP * k:
                    p.write(encode_cmd(target, masks[f], 1)); sent.add(f)
            p.flush()
    p.close()
    info = dict(spawn_fc=spawn_fc, plan_fc0=plan_fc0, sent=len(sent), total=len(masks),
                clear_fc=clear_fc, final_room=(final["rx"], final["ry"]) if final else None)
    if verbose:
        print(f"spawn_fc={spawn_fc}  delivered {len(sent)}/{len(masks)} fc-commands (lead k={k})")
        print(f"-> {'CLEAR at fc=' + str(clear_fc) if cleared else 'NO CLEAR'}"
              f"  (final room {info['final_room']})")
    return cleared, info


def drive_device_chain(port, rooms=((0, 0), (1, 0), (2, 0)), k=2, settle=8, timeout=90.0, verbose=True, fps_out=None):
    """OPEN-LOOP chain: solve the whole run on the sim, then blindly stream the pre-solved masks to the board as
    fc-tagged commands (no twin-in-the-loop, no rebase). The board applies each on its target frame. This is the
    open-loop-Trace shape over the fc-scheduled backend — simpler than `predictive`, and the honest baseline for
    what frame-synced replay does across a multi-room chain (later rooms drift with no feedback to correct it)."""
    import serial
    from harness import FpsMeter
    masks = CL.drive_sim_chain(rooms, record=True, verbose=False, max_frames=max(520, 200 * len(rooms)))["masks"]
    after = (rooms[-1][0] + 1, rooms[-1][1])
    warm = _celeste_warmup()
    meter = FpsMeter(STEP, 30)
    p = serial.Serial(port, 115200, timeout=0.02)
    p.dtr = False; p.rts = True
    time.sleep(0.2); p.reset_input_buffer(); p.rts = False
    t0 = time.monotonic(); buf = b""
    plan_fc0 = None; sent = set(); latest = None; cleared = False; clear_fc = None; final = None; maxroom = (0, 0)
    spawn_t = {}                                  # rc -> wall-clock (from recording) the player reached its spawn
    while time.monotonic() - t0 < timeout:
        chunk = p.read(p.in_waiting or 1)
        if chunk:
            buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            bs = _parse_full(line)
            if bs is None:
                continue
            latest = bs
            meter.add(bs.get("fc"), bs.get("step"), bs.get("draw"))
            if bs["x"] is not None:
                final = bs; maxroom = max(maxroom, (int(bs["rx"]), int(bs["ry"])))
                rc = (int(bs["rx"]), int(bs["ry"]))     # timestamp each room's spawn (for video sync)
                if rc in CL.ROOMS and rc not in spawn_t:
                    sx, sy = CL.ROOMS[rc][1]
                    if abs(bs["x"] - sx) < 1.0 and abs(bs["y"] - sy) < 1.0:
                        spawn_t[rc] = time.monotonic() - t0
            if plan_fc0 is None:
                warm(bs, lambda t, m, h=1: (p.write(encode_cmd(t, m, h)), p.flush()))
                if bs["x"] is not None and (bs["rx"], bs["ry"]) == (0.0, 0.0) \
                        and abs(bs["x"] - SPAWN[0]) < 0.5 and abs(bs["y"] - SPAWN[1]) < 0.5:
                    plan_fc0 = bs["fc"] + STEP * settle
            if plan_fc0 is not None and bs["x"] is not None and (int(bs["rx"]), int(bs["ry"])) == after:
                cleared = True; clear_fc = bs["fc"]; spawn_t["clear"] = time.monotonic() - t0
        if cleared:
            break
        if latest is not None and plan_fc0 is not None:
            F = latest["fc"]
            for g in range(len(masks)):
                if g in sent:
                    continue
                target = plan_fc0 + STEP * g
                if F >= target - STEP * k:
                    p.write(encode_cmd(target, masks[g], 1)); sent.add(g)
            p.flush()
    p.close()
    stats = meter.stats()
    if fps_out is not None and stats:
        fps_out.update(stats)
    tf = os.environ.get("OPENLOOP_TIMES_FILE")   # for render_compare video sync: {rc: spawn wall-clock}
    if tf:
        import json
        json.dump({str(k): v for k, v in spawn_t.items()}, open(tf, "w"))
    names = " -> ".join(CL.ROOMS[rc][0] for rc in rooms)
    if verbose:
        print(f"open-loop {names}: delivered {len(sent)}/{len(masks)} fc-commands")
        print(f"-> {'CLEAR at fc=' + str(clear_fc) if cleared else 'NO CLEAR (reached room %s)' % (maxroom,)}")
        print(meter.summary())
    return dict(cleared=cleared, clear_fc=clear_fc, delivered=len(sent), final=final, maxroom=maxroom, fps=stats)


# ---- TWIN-IN-THE-LOOP predictive closed loop (the board's telemetry drives the plan + rebases on drift) ----
def _parse_full(line):
    """Celeste telemetry incl. spd + djump (for rebasing the twin): T fc step draw x y rx ry spx spy dj."""
    p = line.split()
    if len(p) < 11 or p[0] != b'T':
        return None
    def num(s):
        try: return float(s)
        except Exception: return None
    def integer(s):
        try: return int(s)
        except Exception: return None
    return dict(fc=int(p[1]), step=integer(p[2]), draw=integer(p[3]),   # step/draw us -> fps meter
                x=num(p[4]), y=num(p[5]), rx=num(p[6]), ry=num(p[7]),
                spx=num(p[8]), spy=num(p[9]), dj=num(p[10]))


def _celeste_twin(rooms):
    """The Celeste twin = the sim VM running the closed-loop policy; reset spawns the FIRST room of the chain
    (the game then advances room to room on its own), seed() sets the player from board state."""
    import ctypes
    VM.init(CL.CART)                        # boot the twin VM (reset() = spawn assumes an inited VM)
    VM._lib.sim_peek.argtypes = [ctypes.c_int]; VM._lib.sim_peek.restype = ctypes.c_int
    def seed(bs):
        VM._lib.sim_exec((
            "for o in all(objects) do if o.type==player then "
            f"o.x={bs['x']} o.y={bs['y']} o.spd.x={bs['spx']} o.spd.y={bs['spy']} o.djump={int(bs['dj'])} "
            "end end").encode())
    return dict(reset=lambda: VM.spawn(*rooms[0]), read=CL.read,
                step=lambda m: VM.step_mask(m), seed=seed)


def _celeste_warmup():
    """Title-skip over the fc-scheduled backend (which takes only fc-commands, no raw keys): pulse a jump while
    the title menu is up to press "start", then stop once the game leaves the title and let the spawn animation
    land at (8,96). This is the input-path equivalent of the CELESTE_START Lua autostart — and the one that
    actually works, since a boot-time begin_game()/load_room does not survive the title's own deferred init."""
    O = 16
    st = {"last": -999}
    def warmup(bs, send):
        on_title = bs.get("x") is None and (bs.get("rx"), bs.get("ry")) == (7.0, 3.0)
        if on_title and bs["fc"] - st["last"] >= 8:      # press "start" every few frames until the title clears
            send(bs["fc"] + 6, O, 3); st["last"] = bs["fc"]
    return warmup


def predictive(port, rooms=((0, 0), (1, 0)), k=2, settle=8, timeout=60.0, resync=True, drop_frames=(), verbose=True):
    """Clear a CHAIN of Celeste rooms on the board, board IN the loop. The board reaches room N only by clearing
    N-1, so the twin plays the whole run — room (0,0) -> (1,0) -> ... — and the driver delivers each frame's
    mask fc-tagged, rebasing the twin to the board on any divergence. Default: 100 M -> 200 M. A single-element
    `rooms` (e.g. ((0,0),)) clears just that first room. Returns the live.drive_device_predictive result dict."""
    import live
    twin = _celeste_twin(rooms)
    spawn0 = CL.ROOMS[rooms[0]][1]
    after = (rooms[-1][0] + 1, rooms[-1][1])     # the room reached once every chain room is cleared

    def at_start(bs):                            # the board is at the FIRST room's spawn (static until frame 0)
        return bs["x"] is not None and (bs["rx"], bs["ry"]) == (float(rooms[0][0]), float(rooms[0][1])) \
            and abs(bs["x"] - spawn0[0]) < 0.5 and abs(bs["y"] - spawn0[1]) < 0.5

    def board_done(bs):                          # the whole chain is cleared -> advanced into the room after last
        return bs["x"] is not None and (int(bs["rx"]), int(bs["ry"])) == after

    def diverged(bs, ts):                        # board drifted off the twin's prediction (room or position)
        if bs["x"] is None:
            return False
        if (int(bs["rx"]), int(bs["ry"])) != (int(ts["rx"]), int(ts["ry"])):
            return True
        return abs(bs["x"] - ts["x"]) > 6 or abs(bs["y"] - ts["y"]) > 6

    def twin_done(st):                           # bound the twin's forward run: chain cleared or fell out
        return (int(st["rx"]), int(st["ry"])) == after or st["y"] > 130

    r = live.drive_device_predictive(port, lambda: CL.Chain(rooms), twin, _parse_full, at_start, board_done,
                                     diverged, encode_cmd=encode_cmd, twin_done=twin_done, k=k, settle=settle,
                                     timeout=timeout, resync=resync, drop_frames=drop_frames,
                                     max_frames=max(520, 200 * len(rooms)),
                                     warmup=_celeste_warmup(), has_state=lambda bs: bs["x"] is not None,
                                     verbose=verbose)
    return r


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = args[0] if args else "/dev/ttyUSB0"
    k = 2
    for a in sys.argv:
        if a.startswith("--k"):
            k = int(a.split("=", 1)[1]) if "=" in a else int(sys.argv[sys.argv.index(a) + 1])
    rooms = (((0, 0),) if "--room0" in sys.argv
             else ((0, 0), (1, 0), (2, 0), (3, 0)) if "--to400" in sys.argv
             else ((0, 0), (1, 0), (2, 0)) if "--to300" in sys.argv
             else ((0, 0), (1, 0)))
    if "--openloop" in sys.argv:
        r = drive_device_chain(port, rooms=rooms, k=k, timeout=40 + 25 * len(rooms))
        names = " -> ".join(CL.ROOMS[rc][0] for rc in rooms)
        print(f"PASS: {names} cleared OPEN-LOOP on the board (fc-scheduled replay)." if r["cleared"]
              else f"reached room {r['maxroom']} open-loop (later rooms drift with no feedback).")
        return 0 if r["cleared"] else 1
    if "--predictive" in sys.argv or "--chain" in sys.argv:
        r = predictive(port, rooms=rooms, k=k, timeout=40 + 25 * len(rooms))
        cleared = r["cleared"]
        names = " -> ".join(CL.ROOMS[rc][0] for rc in rooms)
        print(f"PASS: {names} cleared CLOSED-LOOP on the board (twin-in-the-loop predictive)." if cleared
              else f"FAIL: {names} not cleared (predictive).")
        return 0 if cleared else 1
    cleared, _ = drive_device(port, k=k)
    print("PASS: room (0,0) cleared closed-loop on the board via fc-scheduled input." if cleared
          else "FAIL: room (0,0) not cleared — check the build flags (INPUT_BACKEND=scheduled, CELESTE_START, "
               "TELEMETRY) and that <port> is the board (CP2104).")
    return 0 if cleared else 1


if __name__ == "__main__":
    sys.exit(main())
