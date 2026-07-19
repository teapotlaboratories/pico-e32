#!/usr/bin/env python3
"""Render Celeste's solution Trace on the sim to an mp4 (pixel-perfect) — a thin adapter over the shared
`../render.py`. Celeste's segment reset = `spawn(rx, ry)`; a room clears when `(rx, ry)` advances.

    python3 test/playtest/celeste/render_run.py [out.mp4] [trace.json]   # default trace: solution.trace.json
"""
import sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))                 # render.py, trace.py
sys.path.insert(0, os.path.join(HERE, "..", "fake08-sim"))   # fake08sim
import fake08sim as VM
import render as R
from trace import Trace

REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "sim_run.mp4"
    tracefile = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "solution.trace.json")
    tr = Trace.load(tracefile)
    path, n = R.render(
        os.path.join(REPO, "assets", "celeste.p8"), tr,
        reset=lambda seg: VM.spawn(int(seg.meta['rx']), int(seg.meta['ry'])),
        out=out,
        stop_on_clear=lambda st, seg: (st['rx'], st['ry']) != (int(seg.meta['rx']), int(seg.meta['ry'])))
    print(f"wrote {path} ({n} frames)")


if __name__ == "__main__":
    sys.exit(main())
