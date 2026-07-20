#!/usr/bin/env python3
"""Pico Racer (M9) device/sim driver. The racer is a NON-platformer, continuous-control cart (one timed run,
no room clears — it ends when the on-cart `clock` hits 0), so its primary solution is a LIVE CLOSED-LOOP
POLICY, not an open-loop trace: `policy(state) -> mask` reads the state each frame and steers in response.
The SAME policy runs on both targets via ../live.py — `drive_sim` reads state off the VM, `drive_device` reads
the identical state off serial telemetry and sends buttons live. Feedback self-corrects, so rnd/fps/timing
drift is absorbed (an open-loop replay of a 49-second steering run would compound errors on hardware — that's
why the device driver is closed-loop; `--replay` keeps the fragile open-loop path for comparison).

RUN:
    python3 test/playtest/pico_racer/racer_playtest.py <board-port>   # CLOSED-LOOP live on the board (default)
    python3 test/playtest/pico_racer/racer_playtest.py --sim          # the SAME policy, live on the sim
    python3 test/playtest/pico_racer/racer_playtest.py <board> --replay [--trace=<f>]   # open-loop trace (fragile)

REQUIRED FIRMWARE (build + flash from the repo root; `-D RACER=1` embeds the racer as the flash cart):
    make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<board> \
      DEFS='-D FORCE_FLASH_CART=1 -D RACER=1 -D TELEMETRY=1 -D TELEMETRY_HOST_CFG=1 \
            -D TELEMETRY_BAUD=921600 -D TELEMETRY_BINARY=1 -D TELEMETRY_BINARY_BYTES=40 -D RND_SEED=39 \
            -D INPUT_BACKEND=serial -D INPUT_HOLD_FRAMES=2 -D SHOW_FPS=1 -D CENTER_GAME=1'
`TELEMETRY_HOST_CFG=1` makes the firmware cart-agnostic: the telemetry TAIL isn't baked in — this driver sends
`RACER_TAIL_BIN` (a poke-tail, below) at startup via the CFG? handshake, so no per-cart firmware. The racer's
dodge is latency-sensitive, so the device path defaults to the two levers that tightened it: `TELEMETRY_BAUD`
(runtime-switch to 921600 after the handshake — less serialization jitter) and `TELEMETRY_BINARY` (ship the
observation as raw sync-framed int16s from 0x4300 instead of an ASCII line — no fix32->string/GC per frame).
INPUT_HOLD_FRAMES=**2** (Celeste uses 1): the serial backend decrements a key's hold once per Step and a
30 fps cart is two Steps per game-frame, so a continuously-held key (accelerate) needs =2 to survive both.

Telemetry (binary): `0xAA 0x55` + fc(i32) + step_us(u16) + draw_us(u16) + `BINARY_FIELD_BYTES` of int16 fields
(the exact `solve._FIELDS` observation, poked at 0x4300 by `RACER_TAIL_BIN`). See `unpack_binary` below."""
import sys, os, struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))                 # find live.py / harness.py / trace.py
sys.path.insert(0, os.path.join(HERE, "..", "fake08-sim"))   # fake08sim (sim reads)
sys.path.insert(0, HERE)                                      # find solve.py (choose_mask / make_reader)

from solve import PARAMS, _state_lua, decode_binary, BINARY_FIELD_BYTES  # observation layout (one source)

DEFAULT_TRACE = os.path.join(HERE, "solution.trace.json")
CART = os.path.join(REPO, "assets", "Pico Racer.p8.png")
START_SENDS = ()

LEFT, RIGHT, UP, DOWN, O, X = 1, 2, 4, 8, 16, 32

# The racer's telemetry TAIL — a Lua expression the (cart-agnostic) firmware appends after `T <fc> <step> <draw> `,
# sent at startup via the CFG? handshake (firmware `-D TELEMETRY_HOST_CFG=1`). It streams everything the
# controller needs to DODGE traffic and take curves: steering (px/speed/pt), the current + worst-upcoming curve,
# the two nearest cars (obj type 3) and the nearest jump ramp (type 12) from the cart's object queue. The field
# ORDER must match `_TAIL_KEYS` / `parse_line`. (The same reads the sim uses — see solve.py `_state_lua`.)
RACER_TAIL = (
    "(function() local bz=9999 local bx=0 local fnd=0 local bz2=9999 local bx2=0"
    " local rz=9999 local rl=0 local rf=0 local rbd=9999"
    " for o in all(objq) do"
    " if o.t==3 and o.z>-16 then if o.z<bz then bz2=bz bx2=bx bz=o.z bx=o.x fnd=1 elseif o.z<bz2 then bz2=o.z bx2=o.x end"
    " elseif o.t==12 and o.z>-16 and o.z<80 then local l=-o.x/2 local d=abs(l-px) if d<rbd then rbd=d rl=l rz=o.z rf=1 end end end"
    " local n=#trackq local w=abs(trackq[tpos].curve)"
    " for j=1,8 do local c=abs(trackq[((tpos-1+j)%n)+1].curve) if c>w then w=c end end"
    " return px..' '..speed..' '..pt..' '..trackq[tpos].curve..' '..tpos..' '..clock..' '..gamemode..' '..(resetpos and 1 or 0)"
    "..' '..fnd..' '..bz..' '..bx..' '..bz2..' '..bx2..' '..w..' '..rf..' '..rz..' '..rl end)()"
)
# tail field order (after the generic T <fc> <step> <draw> prefix):
_TAIL_KEYS = ("px", "speed", "pt", "cur", "tpos", "clock", "gm", "rp",
              "cfnd", "cz", "cx", "cz2", "cx2", "worst", "rfnd", "rz", "rlane")

# ---- BINARY telemetry (the DEFAULT device path) --------------------------------------------------------------
# Instead of the ASCII tail above, the cart-agnostic firmware (`-D TELEMETRY_BINARY=1`) treats the CFG tail as a
# Lua statement that POKES the observation into scratch RAM 0x4300.., then ships those bytes raw + sync-framed —
# no per-frame fix32->string/GC. `RACER_TAIL_BIN` is exactly `solve._state_lua` (which make_reader/the solver
# already use) flattened to ONE line, because the CFG? handshake is newline-terminated. `unpack_binary` decodes
# the packet the firmware sends: `0xAA 0x55` + fc(i32) + step_us(u16) + draw_us(u16) + the int16 fields.
SYNC = b"\xAA\x55"
PACKET_LEN = 10 + BINARY_FIELD_BYTES                 # 2 sync + 4 fc + 2 step + 2 draw + fields
RACER_TAIL_BIN = _state_lua(PARAMS.get("look", 1)).decode().replace("\n", " ").replace("\r", " ")


def unpack_binary(pkt):
    """DEVICE state source (binary). Decodes one `PACKET_LEN`-byte sync-framed packet into the SAME state dict
    the policy consumes — fc/step/draw from the header, the observation via `solve.decode_binary` (identical
    field order + scaling to the sim reader). Returns None if the packet is malformed."""
    if len(pkt) < PACKET_LEN:
        return None
    fc, step, draw = struct.unpack_from("<iHH", pkt, 2)
    st = decode_binary(pkt[10:10 + BINARY_FIELD_BYTES])
    st["fc"], st["step"], st["draw"] = fc, step, draw
    return st


# ---- the CLOSED-LOOP policy — the SAME controller on sim and device (reads only tail fields) -----------------
# Device control latency: the sim applies each mask on the frame it read; the board is ~1 frame behind (serial
# round-trip + the input pipeline), and the racer's dodge is brutally latency-sensitive (a clean 1-frame lag
# drops the sim from tpos 62 to 27). A "react to cars earlier" margin (bump dodge_z 34->44) cancels the lag.
# The catch: the board's latency is *jittery*, so at 115200 the bump did nothing (tpos 32); once the higher
# TELEMETRY_BAUD tightened the serialization, the SAME bump lifted it 33 -> 43. So it's ON by default and pairs
# with the higher baud. (rnd car layout matches sim<->device, so 62 is the ceiling; the rest is residual jitter.)
DEVICE_LATENCY_BUMP = 10
_choose = _params = None
_dodge_bump = 0
def policy(st):
    """One frame of control. During the countdown / a transition (gamemode != 0) just hold accelerate; otherwise
    run the full controller — cancel the curve + steer to a target lane, DODGE the nearest car(s), seek a jump
    ramp before a river, and ease speed through tight bends. It reads the LIVE state each frame (feedback), so
    it self-corrects on either target. This is the exact `choose_mask` the sim solver uses, with an optional
    react-earlier margin (`_dodge_bump`) to cover the device's control latency: one controller, two front-ends."""
    global _choose, _params
    if st.get("px") is None or (st.get("gm") or 0) != 0:
        return X
    if _choose is None:
        from solve import choose_mask, PARAMS
        _choose, _params = choose_mask, PARAMS
    P = _params if not _dodge_bump else {**_params, "dodge_z": _params["dodge_z"] + _dodge_bump}
    return _choose(st, P)


def make_done():
    """'Run over': the clock hit 0 after the race started, or the cart reached a game-over / finish gamemode."""
    seen = {"started": False}
    def done(s):
        cl, gm = s.get("clock"), (s.get("gm") or 0)
        if cl is not None and cl > 1:
            seen["started"] = True
        if gm in (1, 2, 5):
            return True
        return seen["started"] and cl is not None and cl <= 0
    return done


def parse_line(line):
    """DEVICE state source. Parses `T <fc> <step_us> <draw_us> <tail…>` (tail = `_TAIL_KEYS`, streamed by
    `RACER_TAIL`) into the state dict the policy consumes. None for non-telemetry lines."""
    p = line.split()
    if len(p) < 4 + len(_TAIL_KEYS) or p[0] != b'T':
        return None
    def num(s):
        try: return float(s)
        except Exception: return None
    st = dict(fc=int(p[1]), step=num(p[2]), draw=num(p[3]))
    for i, k in enumerate(_TAIL_KEYS):
        st[k] = num(p[4 + i])
    return st


_sim_read = None
def racer_read():
    """SIM state source — the SAME dict the policy consumes, read straight out of the VM (reuses the sim
    solver's reader, so the fields match the device tail exactly). Adds the fc the fps meter groups on."""
    global _sim_read
    import fake08sim as VM
    if _sim_read is None:
        from solve import make_reader, PARAMS
        _sim_read = make_reader(PARAMS.get("look", 1))
    st = _sim_read()
    st["fc"] = VM.frame_count(); st["step"] = None; st["draw"] = None
    return st


def _srand(seed):
    def reset(_):
        import fake08sim as VM
        VM._lib.sim_exec(("srand(%d)" % seed).encode())
    return reset


# ---- run the policy live -------------------------------------------------------------------------------------
def drive_device(port, timeout=180, verbose=True):
    """CLOSED-LOOP live control on the board (via live.drive_device) -> (final_tpos, fps_stats). Reads BINARY
    telemetry (sync-framed int16s) and turns on the react-earlier margin, the two levers that cover the board's
    ~1-frame, jittery serial control latency. Requires the binary firmware DEFS in the module docstring."""
    import live
    global _dodge_bump
    _dodge_bump = DEVICE_LATENCY_BUMP
    fps = {}
    r = live.drive_device(port, policy, None, make_done(), timeout=timeout,
                          telemetry_config=RACER_TAIL_BIN, fps_out=fps, verbose=verbose,
                          binary=(SYNC, PACKET_LEN, unpack_binary))
    final = r["final"] or {}
    return (final.get("tpos") or 1), (fps or None)


def drive_sim(seed=39, verbose=True):
    """The SAME policy, live on the sim (via live.drive_sim) -> (final_tpos, frames). Proof the closed-loop
    solution is dual-target: no board needed."""
    import live
    r = live.drive_sim(CART, policy, racer_read, make_done(), reset=_srand(seed))
    final = r["final"]
    if verbose:
        print(f"sim closed-loop: tpos {final['tpos']}/141, clock {final['clock']}, {r['count']} frames")
    return final["tpos"], r["count"]


# ---- open-loop trace replay (kept for comparison; fragile on hardware for this cart) -------------------------
def segments_from_trace(path):
    from harness import Segment
    from trace import Trace, mask_to_keys
    tr = Trace.load(path)
    segs = []
    for s in tr.segments:
        plan = [mask_to_keys(m) for m in s.frames]
        done = make_done()
        segs.append(Segment(s.name, plan, (lambda st: st["fc"] is not None), done,
                            (lambda st, n=s.name: f"{n}: reached tpos={st['tpos']} (clock out)")))
    return segs, tr.steps_per_frame


def replay_device(port, tracefile=DEFAULT_TRACE, timeout=180):
    from harness import run
    segments, spf = segments_from_trace(tracefile)
    fps = {}
    n = run(port, parse_line, segments, settle=0, lead=2, steps_per_frame=spf,
            start_sends=START_SENDS, timeout=timeout, done_grace=2, telemetry_config=RACER_TAIL,
            fps_out=fps, verbose=False)
    return n, len(segments), (fps or None)


def main():
    if "--sim" in sys.argv:
        drive_sim()
        return 0
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = args[0] if args else "/dev/ttyUSB0"
    if "--replay" in sys.argv:
        from harness import run
        tracefile = next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--trace=")), DEFAULT_TRACE)
        segments, spf = segments_from_trace(tracefile)
        n = run(port, parse_line, segments, settle=0, lead=2, steps_per_frame=spf,
                start_sends=START_SENDS, timeout=180, done_grace=2, telemetry_config=RACER_TAIL)
        print("PASS" if n >= 1 else "FAIL", "(open-loop trace replay)")
        return 0 if n >= 1 else 1
    tpos, fps = drive_device(port, timeout=180)
    print(f"Pico Racer driven CLOSED-LOOP on the board — reached tpos {tpos:.0f}/141.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
