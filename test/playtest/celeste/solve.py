#!/usr/bin/env python3
"""Celeste solver: drive the shared replay-from-root VM search (../search.py) to clear a room and emit a
replay-able Trace. This is the Celeste ADAPTER — the cart-specific callables (reset / observe / win / dead /
score / signature) and the macro set live here; the search engine is shared and cart-agnostic.

The macro set is reused from the offline twin solver (`celeste_solver/solve.py`); the search here runs on
the EXACT device VM (no twin), so the emitted Trace replays-to-clear on the sim and — same VM, same inputs —
on the device. See ../README.md.

    python3 test/playtest/celeste/solve.py 0 0 -o solution.trace.json   # solve room (0,0)
"""
import sys, os, time, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
PT = os.path.join(HERE, "..")                       # test/playtest (shared: search.py, trace.py)
sys.path.insert(0, PT)
sys.path.insert(0, os.path.join(PT, "fake08-sim"))  # fake08sim
sys.path.insert(0, HERE)                            # celeste_solver
import fake08sim as VM
import search as SR
from trace import Trace, Segment

MACROBIT = {'L': 1, 'R': 2, 'U': 4, 'D': 8, 'O': 16, 'X': 32}   # macro keys -> held-button bits
SPAWN = {(0, 0): [8.0, 96.0], (1, 0): [8.0, 112.0]}             # room -> spawn (x, y)
NAME = {(0, 0): "100 M", (1, 0): "200 M"}


def cands():
    """The proven macro set from the twin solver, as {name: [per-frame mask]}."""
    from celeste_solver.solve import CANDS, macro_frames
    def m(f): return sum(MACROBIT[c] for c in f)
    return {f"{k}:{a}": [m(f) for f in macro_frames(k, a)] for (k, a) in CANDS}


def solve_room(rx, ry, beam=300, depth=18, verbose=True):
    """Run the shared beam search with Celeste's callables. Win = advanced to the next room; dead = the
    player object is gone (killed) or fell off; score = player y (lower = higher climb = better); dedup on
    the twin's proven key (x, y, djump, freeze, sign vx, sign vy)."""
    return SR.beam_search(
        lambda: VM.spawn(rx, ry), VM.read, cands(),
        is_win=lambda st: (st['rx'], st['ry']) != (rx, ry),
        is_dead=lambda st: st['found'] == 0 or st['y'] > 120,
        score=lambda st: st['y'],
        signature=lambda st: (st['x'], st['y'], st['dj'], st['fz'], st['sx'], st['sy']),
        beam=beam, depth=depth, verbose=verbose)


def main():
    ap = argparse.ArgumentParser(description="Celeste solver on the exact VM (replay-from-root)")
    ap.add_argument("rx", nargs="?", type=int, default=0)
    ap.add_argument("ry", nargs="?", type=int, default=0)
    ap.add_argument("-o", "--out", default=None, help="write the solution Trace to this path")
    ap.add_argument("--beam", type=int, default=300)
    ap.add_argument("--depth", type=int, default=18)
    args = ap.parse_args()
    rx, ry = args.rx, args.ry

    VM.init(os.path.join(os.getcwd(), "assets/celeste.p8"))
    print(f"solving celeste room ({rx},{ry}) beam={args.beam} depth={args.depth}...", flush=True)
    t0 = time.monotonic()
    wins = solve_room(rx, ry, args.beam, args.depth)
    print(f"done in {time.monotonic()-t0:.1f}s: {len(wins)} winning routes", flush=True)
    if not wins:
        print("NO SOLUTION (try a larger beam/depth)"); return 1
    frames = wins[0][2]
    print(f"best: {len(frames)} game-frames, best_y={wins[0][0]:.0f}")

    tr = Trace("celeste.p8",
               [Segment(NAME.get((rx, ry), f"room{rx}{ry}"), frames,
                        {"rx": rx, "ry": ry, "spawn": SPAWN[(rx, ry)]})],
               steps_per_frame=2, meta={"solver": "celeste/solve.py replay-from-root VM beam"})
    # self-check: the emitted Trace must replay-to-clear on the sim (the device half is the same input path)
    VM.spawn(rx, ry); cleared = False
    for mm in tr.segments[0].frames:
        VM.step_mask(mm); s = VM.read()
        if (s['rx'], s['ry']) != (rx, ry):
            cleared = True; break
    print("sim replay of emitted trace:", "CLEAR" if cleared else "FAIL")
    if args.out and cleared:
        tr.save(args.out); print(f"wrote {args.out}")
    return 0 if cleared else 1


if __name__ == "__main__":
    sys.exit(main())
