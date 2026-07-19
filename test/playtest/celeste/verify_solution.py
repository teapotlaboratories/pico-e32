#!/usr/bin/env python3
"""Verify test/playtest/celeste/solution.trace.json the way the task demands — SEE it, don't assert it.

For each segment of the Trace:
  1. SIM REPLAY on the EXACT device VM: spawn(rx,ry), step the segment's masks, assert the room (rx,ry)
     advances (the room is cleared). Reports frames-used and the (rx,ry) transition.
  2. LOOK: render a gym.run_filmstrip of the cleared room to a PNG (montage of sampled, labelled frames)
     so a human/agent can confirm the player actually traverses the room and exits the top.

    python3 test/playtest/celeste/verify_solution.py [out_dir]
"""
import sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
PT = os.path.join(HERE, "..")
sys.path.insert(0, PT)
sys.path.insert(0, os.path.join(PT, "fake08-sim"))
import fake08sim as VM
import gym
from trace import Trace

CART = os.path.join(os.getcwd(), "assets/celeste.p8")
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/celeste-verify"   # throwaway PNGs -> /tmp, not the source tree
TRACE = os.path.join(HERE, "solution.trace.json")


def main():
    os.makedirs(OUT, exist_ok=True)
    VM.init(CART)
    tr = Trace.load(TRACE)
    print(f"trace: {tr.cart}, steps_per_frame={tr.steps_per_frame}, {len(tr.segments)} segments, "
          f"{tr.total_frames()} game-frames")
    ok = True
    for si, seg in enumerate(tr.segments):
        rx, ry = int(seg.meta["rx"]), int(seg.meta["ry"])
        spawn = seg.meta["spawn"]

        # 1) SIM REPLAY (assertion): spawn + step masks, require the room to advance.
        st0 = VM.spawn(rx, ry)
        assert (st0["x"], st0["y"]) == (spawn[0], spawn[1]), \
            f"spawn mismatch: got ({st0['x']},{st0['y']}) expected {spawn}"
        cleared = False; used = 0; nxt = (rx, ry); miny = st0["y"]
        for i, m in enumerate(seg.frames):
            VM.step_mask(m); s = VM.read()
            miny = min(miny, s["y"])
            if (s["rx"], s["ry"]) != (rx, ry):
                cleared = True; used = i + 1; nxt = (s["rx"], s["ry"]); break
        status = "CLEAR" if cleared else "FAIL"
        print(f"\n[{seg.name}] room ({rx},{ry}) spawn {spawn}: SIM REPLAY {status}")
        print(f"    frames: {used}/{len(seg.frames)} used, climbed to min_y={miny}, "
              f"room ({rx},{ry}) -> {nxt}")
        ok = ok and cleared

        # 2) LOOK: filmstrip of the run (reset spawns the room; label shows game-frame + y + room).
        png = os.path.join(OUT, f"verify_{seg.name.replace(' ', '')}.png")
        gym.run_filmstrip(
            seg.frames, png, every=6, cols=8, scale=2,
            reset=lambda rx=rx, ry=ry: VM.spawn(rx, ry),
            label=lambda stt, i: f"f{i} y{stt['y']} r{stt['rx']},{stt['ry']}")
        print(f"    filmstrip -> {png}")

    print(f"\nOVERALL: {'PASS - both segments clear on the exact VM' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
