#!/usr/bin/env python3
"""Pico Racer through the TWIN-IN-THE-LOOP predictive closed loop (live.drive_device_predictive) — the CONTINUOUS
cart that exercises the rebase/recovery the frame-precise Celeste room can't. The racer's controller
(solve.choose_mask) is STATELESS, so a rebase is clean: seed the twin's lateral state (px/pt) to the board and
the pure policy re-steers. rnd() is pinned (srand on both twin + device via RND_SEED) so the twin matches the
board; a divergence therefore comes from a missed/late command, which continuous control RECOVERS from.

REQUIRED FIRMWARE:
    make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<board> \\
      DEFS='-D RACER=1 -D RND_SEED=39 -D FORCE_FLASH_CART=1 -D INPUT_BACKEND=scheduled -D TELEMETRY=1 \\
            -D TELEMETRY_HOST_CFG=1 -D SHOW_FPS=1 -D CENTER_GAME=1'
The driver sends `RACER_TAIL` at the CFG? handshake (tpos/px/speed/pt/clock/gamemode — enough to anchor,
detect divergence + the run's end, and rebase; the POLICY runs on the twin's full state, not this).

RUN:  python3 test/playtest/pico_racer/racer_fc_device.py <board-port>
"""
import sys, os, ctypes

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))                 # live, fc_sched, harness
sys.path.insert(0, os.path.join(HERE, "..", "fake08-sim"))   # fake08sim
sys.path.insert(0, HERE)                                     # solve
import fake08sim as VM
from fc_sched import encode_cmd
from solve import CART, SEED, PARAMS, choose_mask, make_reader

LEFT, RIGHT, UP, DOWN, O, X = 1, 2, 4, 8, 16, 32

# CFG telemetry tail (ASCII): the fields the driver needs for anchor / divergence / done / rebase.
RACER_TAIL = "tpos..' '..px..' '..speed..' '..pt..' '..clock..' '..gamemode"


def parse_line(line):
    p = line.split()
    if len(p) < 10 or p[0] != b'T':
        return None
    def num(s):
        try: return float(s)
        except Exception: return None
    def integer(s):
        try: return int(s)
        except Exception: return None
    return dict(fc=int(p[1]), step=integer(p[2]), draw=integer(p[3]),   # generic prefix -> fps meter
                tpos=num(p[4]), px=num(p[5]), speed=num(p[6]),
                pt=num(p[7]), clock=num(p[8]), gm=num(p[9]))


def make_policy():
    """The racer controller as a `policy(state)->mask` — STATELESS: hold X through the countdown/title
    (gamemode != 0), otherwise the full dodge/steer controller. (make_policy is a factory for the driver;
    stateless, so every call returns the same pure function.)"""
    def pol(s):
        return X if (s.get("gm") or 0) != 0 else choose_mask(s, PARAMS)
    return pol


def _racer_twin(seed=SEED, look=None):
    """Twin = the racer sim VM, rnd pinned. seed() re-syncs the recoverable lateral state (px, pt) to the board."""
    look = PARAMS["look"] if look is None else look
    VM._lib.sim_exec.argtypes = [ctypes.c_char_p]
    reader = make_reader(look)
    def reset():
        VM.init(CART)
        VM._lib.sim_exec(("srand(%d)" % seed).encode())      # pin rnd (matches the device's RND_SEED)
    def resync(bs):
        VM._lib.sim_exec(("px=%r pt=%r" % (bs["px"], bs["pt"])).encode())
    return dict(reset=reset, read=reader, step=lambda m: VM.step_mask(m), seed=resync)


def make_done():
    """Run over: the cart hit a game-over gamemode, or the clock ran out after the race started."""
    seen = {"started": False}
    def done(s):
        gm, cl = (s.get("gm") or 0), s.get("clock")
        if gm == 0 and cl is not None and cl > 1:
            seen["started"] = True
        if gm in (1, 2, 5):
            return True
        return seen["started"] and cl is not None and cl <= 0
    return done


def twin_done_state(s):
    """Bound the twin's forward run (grow) — stop before the VM would tear down at game-over."""
    return (s.get("gm") or 0) in (1, 2, 5)


def predictive(port, k=2, settle=0, timeout=90.0, resync=True, drop_frames=(), verbose=True):
    """Drive the racer on the board via the predictive closed loop; return the result dict (adds final tpos)."""
    import live
    twin = _racer_twin()

    def at_start(bs):
        return bs.get("fc") is not None and bs.get("gm") is not None      # first telemetry frame with state

    def diverged(bs, ts):
        if bs.get("px") is None or ts.get("px") is None:
            return False
        return abs(bs["px"] - ts["px"]) > 4.0 or abs((bs.get("tpos") or 0) - (ts.get("tpos") or 0)) > 3

    r = live.drive_device_predictive(port, make_policy, twin, parse_line, at_start, make_done(), diverged,
                                     encode_cmd=encode_cmd, twin_done=twin_done_state, k=k, settle=settle,
                                     timeout=timeout, resync=resync, drop_frames=drop_frames,
                                     telemetry_config=RACER_TAIL, max_frames=6000, verbose=verbose)
    r["final_tpos"] = (r.get("final") or {}).get("tpos")
    if verbose:
        print(f"racer predictive: final tpos={r['final_tpos']}")
    return r


def _sim_selfcheck():
    """Validate the twin + policy off the board: run the policy on the twin and report how far it drives."""
    twin = _racer_twin()
    pol = make_policy()
    twin["reset"](); st = twin["read"](); tpos = st["tpos"]; n = 0
    while n < 6000:
        if twin_done_state(st):
            break
        twin["step"](pol(st)); st = twin["read"](); tpos = max(tpos, st["tpos"]); n += 1
        if make_done()(st):
            break
    print(f"[sim self-check] twin+policy drove to tpos {tpos}/141 in {n} frames "
          f"(clock={st['clock']}, gm={st['gm']})")
    return tpos


def main():
    if "--selfcheck" in sys.argv:
        _sim_selfcheck(); return 0
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = args[0] if args else "/dev/ttyUSB0"
    r = predictive(port)
    print("PASS: racer driven CLOSED-LOOP on the board (twin-in-the-loop predictive), "
          f"tpos {r['final_tpos']}." if (r["final_tpos"] or 0) > 20 else "FAIL: racer did not drive.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
