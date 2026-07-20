#!/usr/bin/env python3
"""Verify test/playtest/pico_racer/solution.trace.json — SEE it, don't just assert it.

This is the authoritative, OPEN-LOOP check. It runs as a FRESH process doing exactly ONE VM.init (fake-08
re-inits leak prior VM state, so only the first init is canonical), pins rnd with srand(seed) from the
Trace meta, then steps the segment's recorded masks with NO closed-loop feedback. It:
  1. asserts the final track position `tpos` equals what solve.py banked, and
  2. renders a filmstrip PNG (to /tmp) sampled from that very replay so a human/agent can watch the car
     drive the road, bank goals (the clock jumps), and stay on-track.

    python3 test/playtest/pico_racer/verify_solution.py [out_dir]
"""
import sys, os, ctypes, subprocess, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
PT = os.path.join(HERE, "..")
sys.path.insert(0, PT)
sys.path.insert(0, os.path.join(PT, "fake08-sim"))
import fake08sim as VM
import gym                      # read-only: reuse its RGB->PNG helper
from trace import Trace

CART = os.path.join(os.getcwd(), "assets/Pico Racer.p8.png")
TRACE = os.path.join(HERE, "solution.trace.json")
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pico-racer-verify"

VM._lib.sim_exec.argtypes = [ctypes.c_char_p]
VM._lib.sim_peek.argtypes = [ctypes.c_int]; VM._lib.sim_peek.restype = ctypes.c_int


def _s16(a):
    raw = VM._lib.sim_peek(a) + 256 * VM._lib.sim_peek(a + 1)
    return raw - 65536 if raw >= 32768 else raw


def geti(expr):
    # read-only scratch-RAM peek: state-neutral (only touches 0x4300.., which the cart never reads)
    VM._lib.sim_exec(("do local v=flr(%s) poke2(0x4300,v) end" % expr).encode())
    return _s16(0x4300)


def main():
    os.makedirs(OUT, exist_ok=True)
    tr = Trace.load(TRACE)
    seg = tr.segments[0]
    seed = int(seg.meta.get("seed", tr.meta.get("srand_seed")))
    expect = int(seg.meta["final_tpos"])
    frames = seg.frames
    print(f"trace: {tr.cart}, steps_per_frame={tr.steps_per_frame}, seed={seed}, "
          f"{len(frames)} game-frames, claims final tpos={expect}")

    # --- fresh single init + srand, then OPEN-LOOP replay of the recorded masks ---
    VM.init(CART)
    VM._lib.sim_exec(("srand(%d)" % seed).encode())

    every = max(1, len(frames) // 48)     # ~48 sampled tiles across the whole run
    tmp = tempfile.mkdtemp(prefix="racer_verify_")
    tiles = []
    try:
        for i, m in enumerate(frames):
            VM.step_mask(m)
            if i % every == 0:
                VM.draw()                  # state-neutral: renders the already-computed framebuffer
                tpos = geti("tpos"); clock = geti("clock"); spd = geti("speed*100")
                tile = os.path.join(tmp, f"{len(tiles):04d}.png")
                gym._rgb_to_png(VM.frame_rgb(), tile, 2)
                subprocess.run(["convert", tile, "-background", "black", "-fill", "white",
                                "-pointsize", "10", "-gravity", "South", "-splice", "0x13",
                                "-annotate", "+0+1", f"f{i} t{tpos} c{clock}", tile], check=True)
                tiles.append(tile)
        final_tpos = geti("tpos"); final_clock = geti("clock")
        png = os.path.join(OUT, "verify_run.png")
        subprocess.run(["montage", *tiles, "-tile", "8x", "-geometry", "+2+2",
                        "-background", "gray20", png], check=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    ok = (final_tpos == expect)
    print(f"\nOPEN-LOOP REPLAY: final tpos={final_tpos}/141, clock={final_clock}")
    print(f"    expected tpos={expect} -> {'MATCH' if ok else 'MISMATCH'}")
    print(f"    filmstrip -> {png}")
    print(f"\nOVERALL: {'PASS - trace replays deterministically to tpos %d' % final_tpos if ok else 'FAIL'}")
    assert ok, f"replay tpos {final_tpos} != expected {expect}"
    return 0


if __name__ == "__main__":
    sys.exit(main())
