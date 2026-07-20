#!/usr/bin/env python3
"""Pico Racer solver (milestone M9) — develop a closed-loop driving controller on the exact device VM and
emit an OPEN-LOOP, replay-able Trace.

The cart ("Pico Racer" by kometbomb) is a pseudo-3D racer. There are no player/room objects, so the shared
Celeste `read()` is useless; racer globals are read DIRECTLY out of the VM with exec+peek into scratch RAM
0x4300.. (which the cart never touches). The controller reads `px` (lateral offset), `speed`, `pt`
(steering velocity), the current/upcoming curve, and the object queue each frame, then decides a held-button
mask. Because the sim is deterministic, the recorded per-frame masks replay to the exact same run.

Objective is SCORE-MAXIMIZATION: reach the highest track segment `tpos` before the countdown `clock` hits 0.
The key mechanic is that passing a *goal* gate (object type 16) adds 32 s (960 frames) to the clock, so the
way to go far is to drive fast and on-road, banking each goal to keep the clock topped up.

Controller = three cooperating rules:
  1. STEER  keep `px` near a target lane via a target steering velocity that cancels the curve's push
            (`xpush = curve*32*speed`) plus a proportional pull to the target: `pt* = xpush + K*(px-target)`.
  2. EASE    per-curve speed cap so the steering (whose authority `pt` saturates at +-2) can hold the line
            through tight bends; only genuinely un-holdable curves (verytight) actually slow us.
  3. AVOID   read the object queue: steer AWAY from an approaching car's lane (cars quarter your speed on
            contact), and steer INTO the nearest jump-ramp lane before a river (being airborne makes you
            river-immune; a river otherwise quarters your speed).

Determinism: fake-08 seeds `rnd()` from wall-clock time on every cart load (vm.cpp), and the cart spawns
cars/obstacles with `rnd()`, so a raw run is non-reproducible. We pin it with `srand(SEED)` right after
`VM.init` (baked into the Trace meta and re-applied by the verifier). We also run exactly ONE `VM.init` per
process and prepend a neutral frame so the start-button edge is clean regardless of prior process state.

    python3 test/playtest/pico_racer/solve.py            # solve with the chosen seed -> solution.trace.json
    python3 test/playtest/pico_racer/solve.py --seed 39  # pick a different rnd seed
"""
import sys, os, ctypes, struct, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
PT = os.path.join(HERE, "..")                       # test/playtest (shared: trace.py)
sys.path.insert(0, PT)
sys.path.insert(0, os.path.join(PT, "fake08-sim"))  # fake08sim
import fake08sim as VM
from trace import Trace, Segment

CART = os.path.join(os.getcwd(), "assets/Pico Racer.p8.png")
LEFT, RIGHT, UP, DOWN, O, X = 1, 2, 4, 8, 16, 32

# --- default seed + controller gains (tuned on the sim; see README) ---
SEED = 39
PARAMS = dict(K=0.45, hold=2.0, look=1, dodge_z=34, band=8.0, lim=14.0, brake_over=0.35, ramp_z=60)

# fake-08's sim_exec runs Lua in the cart sandbox; sim_peek reads a byte. We poke 16-bit values into the
# scratch page 0x4300.. and read them back (little-endian, signed). These reads are state-neutral: they only
# touch scratch RAM the cart never reads, so the closed-loop run and its open-loop replay agree exactly.
VM._lib.sim_exec.argtypes = [ctypes.c_char_p]
VM._lib.sim_peek.argtypes = [ctypes.c_int]; VM._lib.sim_peek.restype = ctypes.c_int


def _s16(a):
    raw = VM._lib.sim_peek(a) + 256 * VM._lib.sim_peek(a + 1)
    return raw - 65536 if raw >= 32768 else raw


def geti(expr):
    VM._lib.sim_exec(("do local v=flr(%s) poke2(0x4300,v) end" % expr).encode())
    return _s16(0x4300)


# One Lua snippet reads the whole observation per frame: scalar globals, the worst curve over the next LOOK
# segments (for the speed cap), the two nearest approaching cars, the jump ramp whose lane is nearest us,
# and the nearest river. Scaled to fixed-point and poked into 0x4300.. as 16-bit words.
def _state_lua(look):
    return ("""do
 local bz=9999 local bx=0 local fnd=0 local bz2=9999 local bx2=0
 local rz=9999 local rlane=0 local rfnd=0 local rbestd=9999
 local vz=9999 local vfnd=0
 for o in all(objq) do
  if o.t==3 and o.z>-16 then
   if o.z<bz then bz2=bz bx2=bx bz=o.z bx=o.x fnd=1
   elseif o.z<bz2 then bz2=o.z bx2=o.x end
  elseif o.t==12 and o.z>-16 and o.z<80 then
   local lane=-o.x/2 local d=abs(lane-px)
   if d<rbestd then rbestd=d rlane=lane rz=o.z rfnd=1 end
  elseif o.t==6 and o.z>-16 and o.z<vz then vz=o.z vfnd=1 end
 end
 local n=#trackq local worst=abs(trackq[tpos].curve)
 for j=1,%d do local c=abs(trackq[((tpos-1+j)%%n)+1].curve) if c>worst then worst=c end end
 poke2(0x4300,flr(px*16)) poke2(0x4302,flr(speed*256)) poke2(0x4304,flr(pt*256))
 poke2(0x4306,flr(trackq[tpos].curve*10000)) poke2(0x4308,tpos) poke2(0x430a,clock)
 poke2(0x430c,gamemode) poke2(0x430e,resetpos and 1 or 0)
 poke2(0x4310,flr(bz)) poke2(0x4312,flr(bx)) poke2(0x4314,fnd)
 poke2(0x4316,flr(bz2)) poke2(0x4318,flr(bx2)) poke2(0x431a,flr(worst*10000))
 poke2(0x431c,flr(rz)) poke2(0x431e,flr(rlane*16)) poke2(0x4320,rfnd)
 poke2(0x4322,flr(vz)) poke2(0x4324,vfnd) poke2(0x4326,flr(plrht*16))
end""" % look).encode()


# The observation layout — ONE source of truth for the field order + fixed-point scale. `_state_lua` pokes
# exactly these int16s (in this order) at 0x4300, 0x4302, …; the SIM reader decodes them from scratch RAM and
# the DEVICE's binary telemetry decodes the same bytes off the wire (the firmware ships `BINARY_FIELD_BYTES`
# raw from 0x4300). Keep this list in lock-step with the poke2() order in `_state_lua`. scale None = integer.
_FIELDS = (
    ("px", 16.0), ("speed", 256.0), ("pt", 256.0), ("cur", 10000.0), ("tpos", None), ("clock", None),
    ("gm", None), ("rp", None), ("cz", None), ("cx", None), ("cfnd", None), ("cz2", None), ("cx2", None),
    ("worst", 10000.0), ("rz", None), ("rlane", 16.0), ("rfnd", None), ("vz", None), ("vfnd", None),
    ("plrht", 16.0),
)
BINARY_FIELD_BYTES = 2 * len(_FIELDS)      # bytes the firmware ships from 0x4300 (== -D TELEMETRY_BINARY_BYTES)


def _decode(vals):
    """int16 words (in `_FIELDS` order) -> the state dict, applying each field's fixed-point scale."""
    return {name: (v if scale is None else v / scale) for (name, scale), v in zip(_FIELDS, vals)}


def decode_binary(body):
    """Decode the device's BINARY telemetry body — the `BINARY_FIELD_BYTES` little-endian int16s the firmware
    copies straight from 0x4300 — into the SAME state dict the sim reader returns. Used by the device driver."""
    return _decode(struct.unpack("<%dh" % len(_FIELDS), body[:BINARY_FIELD_BYTES]))


def make_reader(look):
    lua = _state_lua(look)

    def read():
        VM._lib.sim_exec(lua)
        return _decode([_s16(0x4300 + 2 * i) for i in range(len(_FIELDS))])
    return read


def choose_mask(s, p):
    """The closed-loop policy for one game-frame of gameplay (gamemode 0) -> held-button mask."""
    px, speed, pt, cur = s['px'], s['speed'], s['pt'], s['cur']
    K, hold, lim, band = p['K'], p['hold'], p['lim'], p['band']
    target = 0.0
    if s['rfnd'] and s['rz'] < p['ramp_z']:
        # a jump ramp is near: steer to its lane so we launch and clear the coming river un-slowed
        target = max(-lim, min(lim, s['rlane']))
    elif s['cfnd'] and s['cz'] < p['dodge_z']:
        # a car is approaching: aim a lane clear of its collision band (+-6 px around -carx/2)
        dc = -s['cx'] / 2.0
        target = dc - band if dc >= 0 else dc + band
        target = max(-lim, min(lim, target))
        if abs(target - dc) < 6.5:                       # clamp pushed us back into it -> flip side
            target = dc + band if dc < 0 else dc - band
            target = max(-lim, min(lim, target))
        if s['cz2'] < p['dodge_z']:                      # second car: nudge further from its lane too
            dc2 = -s['cx2'] / 2.0
            if abs(target - dc2) < 6.5:
                target += band if target >= dc2 else -band
                target = max(-lim, min(lim, target))
    xpush = cur * 32 * speed
    pt_target = xpush + K * (px - target)
    m = X | (RIGHT if pt < pt_target else LEFT)          # X = accelerate; steer toward the target lane
    cap = 3.19 if s['worst'] < 1e-6 else min(3.19, hold / (s['worst'] * 32))
    if speed > cap:                                      # ease so the line stays holdable through the bend
        m = ((m & ~X) | DOWN) if speed > cap + p['brake_over'] else (m & ~X)
    return m


def drive(seed=SEED, params=PARAMS, cap=8000):
    """One deterministic run of the controller. Returns (masks, stats). masks[0] is a neutral frame; the
    rest are the per-game-frame held masks including the countdown."""
    read = make_reader(params['look'])
    VM.init(CART)
    VM._lib.sim_exec(("srand(%d)" % seed).encode())      # pin rnd so cars/obstacles are reproducible
    masks = [0]; VM.step_mask(0)                          # neutral frame -> clean start-button edge
    maxtpos = 1; offroad = 0; resets = 0
    for _ in range(cap):
        s = read()
        if s['gm'] in (1, 2, 5):                          # game over / completed / map screen
            break
        m = X if s['gm'] != 0 else choose_mask(s, params)  # gamemode!=0: countdown/paused -> just hold X
        if abs(s['px']) >= 20:
            offroad += 1
        if s['rp']:
            resets += 1
        maxtpos = max(maxtpos, s['tpos'])
        VM.step_mask(m); masks.append(m)
        if s['clock'] <= 0 and s['speed'] <= 0.01:
            break
    stats = dict(tpos=geti("tpos"), clock=geti("clock"), maxtpos=maxtpos,
                 offroad=offroad, resets=resets, frames=len(masks))
    return masks, stats


def main():
    ap = argparse.ArgumentParser(description="Pico Racer solver on the exact fake-08 VM")
    ap.add_argument("--seed", type=int, default=SEED, help="rnd seed pinned via srand (fixes car spawns)")
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "solution.trace.json"))
    args = ap.parse_args()

    print(f"driving Pico Racer on the exact VM (seed={args.seed}) ...", flush=True)
    masks, st = drive(args.seed)
    print(f"done: final tpos={st['tpos']}/141, clock={st['clock']}, {st['frames']} game-frames, "
          f"off-road frames={st['offroad']}, crashes={st['resets']}")

    tr = Trace(
        "Pico Racer.p8.png",
        [Segment("run", masks, {"seed": args.seed, "final_tpos": st['tpos'],
                                "final_clock": st['clock'], "neutral_prefix": 1})],
        steps_per_frame=2,
        meta={"solver": "pico_racer/solve.py closed-loop controller on fake-08 VM",
              "srand_seed": args.seed, "final_tpos": st['tpos'], "objective": "max tpos before clock<=0"})
    tr.save(args.out)
    print(f"wrote {args.out}")
    print("NOTE: replay is deterministic only under a fresh single VM.init + srand(seed); "
          "run verify_solution.py (a fresh process) for the authoritative open-loop check.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
