#!/usr/bin/env python3
"""Generic replay-from-root solver on the EXACT device VM (test/playtest/fake08-sim).

Beam-searches over macro-actions; every candidate is evaluated by `spawn + replay(prefix)` on the real VM
— no savestate, no physics twin. The winning input is emitted as a portable Trace (test/playtest/trace.py)
which, by construction, replays-to-clear on the sim, and — same VM, same inputs — on the device too.

Engine is cart-agnostic: a cart adapter supplies {spawn, candidate macros, win/dead/signature from the
VM's read()}. Celeste is the reference adapter (reuses celeste_solver's proven macro set). Run:

    python3 test/playtest/search.py celeste 0 0 -o solution.trace.json   # solve room (0,0)

Perf note: replay-from-root re-plays a node's whole prefix per expansion, so cost is
beam * |cands| * prefix_len per depth. Fine for room-sized problems; VM savestates (parked) would make it
O(1) per node. Keep the beam modest.
"""
import sys, os, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "fake08-sim"))
import fake08sim as VM
from trace import Trace, Segment


# ---- generic beam engine ------------------------------------------------------------------------------
def beam_search(rx, ry, cand_masks, *, beam=300, depth=18, extra_after_win=2, verbose=True):
    """Return winning routes as (best_miny, n_frames, frames) — frames = the per-game-frame mask list that
    clears the room. `cand_masks` is {name: [mask,...]} (a macro -> its per-frame held masks)."""
    cands = list(cand_masks.items())
    B = [([], 200.0)]                     # (prefix_masks, best_min_y)
    seen = set(); wins = []; first = None
    for d in range(depth):
        t0 = time.monotonic(); nxt = []
        for prefix, bm in B:
            for _, cm in cands:
                pm = prefix + cm
                out, st, miny, wl = _eval(rx, ry, pm, len(cm))
                m = min(bm, miny)
                if out == 'win':
                    wins.append((m, wl, pm[:wl])); continue
                if out == 'dead':
                    continue
                key = (st['x'], st['y'], st['dj'], st['fz'], st['sx'], st['sy'])
                if key in seen:
                    continue
                seen.add(key); nxt.append((pm, m))
        nxt.sort(key=lambda s: s[1]); B = nxt[:beam]
        if verbose:
            print(f"  depth {d+1}: beam={len(B)} wins={len(wins)} "
                  f"best_min_y={B[0][1] if B else 999:.0f}  ({time.monotonic()-t0:.1f}s)", flush=True)
        if wins and first is None:
            first = d
        if not B:
            break
        if first is not None and d >= first + extra_after_win:
            break
    wins.sort(key=lambda w: (w[1], w[0]))       # fewest frames, then highest climb
    return wins


def _eval(rx, ry, prefix_masks, new_len):
    """Replay a prefix from the room spawn on the VM. The prefix minus its last macro was already validated
    alive at an earlier depth and the VM is deterministic, so batch-step that part with NO reads (fast) and
    only per-frame read the `new_len` new frames (accurate outcome + min_y). -> (outcome, state, min_y, frames)."""
    VM.spawn(rx, ry)
    n = len(prefix_masks); head = n - new_len
    if head > 0:
        VM.steps(prefix_masks[:head])                      # replay the validated prefix, no reads
    miny = 200.0; st = None
    for j in range(head, n):
        VM.step_mask(prefix_masks[j]); st = VM.read()
        if (st['rx'], st['ry']) != (rx, ry):
            return 'win', st, miny, j + 1                  # crossed into the next room
        if st['found'] == 0 or st['y'] > 120:
            return 'dead', st, miny, j + 1                 # killed (object gone) or fell off
        miny = min(miny, st['y'])
    return 'alive', st, miny, n


# ---- Celeste adapter ----------------------------------------------------------------------------------
def celeste_cands():
    """The proven macro set from celeste_solver, as {name: [per-frame mask]}."""
    sys.path.insert(0, os.path.join(HERE, "celeste"))
    from celeste_solver.solve import CANDS, macro_frames
    MACROBIT = {'L': 1, 'R': 2, 'U': 4, 'D': 8, 'O': 16, 'X': 32}   # macro keys -> held-button bits
    def m(f): return sum(MACROBIT[c] for c in f)
    return {f"{k}:{a}": [m(f) for f in macro_frames(k, a)] for (k, a) in CANDS}


CELESTE_SPAWN = {(0, 0): [8.0, 96.0], (1, 0): [8.0, 112.0]}   # room -> spawn (x,y)
CELESTE_NAME = {(0, 0): "100 M", (1, 0): "200 M"}


def main():
    cart = sys.argv[1] if len(sys.argv) > 1 else "celeste"
    rx, ry = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (0, 0)
    out = None
    for a in sys.argv:
        if a.startswith("-o"):
            out = sys.argv[sys.argv.index(a) + 1] if a == "-o" else a[2:]
    if cart != "celeste":
        print("only the celeste adapter exists so far"); return 2

    def _opt(flag, default):
        for a in sys.argv:
            if a.startswith(flag + "="):
                return int(a.split("=", 1)[1])
        return default
    beam, depth = _opt("--beam", 300), _opt("--depth", 18)

    VM.init(os.path.join(os.getcwd(), "assets/celeste.p8"))
    print(f"solving celeste room ({rx},{ry}) on the exact VM (replay-from-root, beam={beam} depth={depth})...", flush=True)
    t0 = time.monotonic()
    wins = beam_search(rx, ry, celeste_cands(), beam=beam, depth=depth)
    print(f"done in {time.monotonic()-t0:.1f}s: {len(wins)} winning routes", flush=True)
    if not wins:
        print("NO SOLUTION found (try a larger beam/depth)"); return 1
    best = wins[0]
    frames = best[2]
    print(f"best: {len(frames)} game-frames, min_y={best[0]:.0f}")

    tr = Trace("celeste.p8",
               [Segment(CELESTE_NAME.get((rx, ry), f"room{rx}{ry}"), frames,
                        {"rx": rx, "ry": ry, "spawn": CELESTE_SPAWN[(rx, ry)]})],
               steps_per_frame=2, meta={"solver": "search.py replay-from-root VM beam"})
    # self-check: the emitted trace must replay-to-clear on the sim (the device half is the same input path)
    seg = tr.segments[0]
    VM.spawn(rx, ry); cleared = False
    for mm in seg.frames:
        VM.step_mask(mm); s = VM.read()
        if (s['rx'], s['ry']) != (rx, ry): cleared = True; break
    print("sim replay of emitted trace:", "CLEAR" if cleared else "FAIL")
    if out and cleared:
        tr.save(out); print(f"wrote {out}")
    return 0 if cleared else 1


if __name__ == "__main__":
    sys.exit(main())
