#!/usr/bin/env python3
"""Produce (and self-verify) test/playtest/celeste/solution.trace.json — a replay-able Trace that clears
Celeste rooms 1 and 2 ("100 M" room (0,0) -> "200 M" room (1,0) -> "300 M").

Method (the solver-agent flow, isolated under celeste/):
  1. DERIVE each room's route by re-running the offline physics-twin beam search (celeste_solver/solve.py's
     `beam_solve_fast`) from scratch — genuinely searched, not copied. The twin models Celeste's
     move-before-update ordering + the 2-frame dash freeze, so its per-game-frame plan is frame-synced to
     the VM's frame counter.
  2. CONVERT the winning route to a per-game-frame key plan (`plan_from_route`).
  3. VERIFY that plan on the EXACT device VM (fake08-sim): spawn(rx,ry), step the masks, require the room
     (rx,ry) to advance. Candidates are tried best-first; the first that clears on the real VM is accepted.
     (This is the acceptance gate — a route is only used if the real VM, not just the twin, clears on it.)
  4. EMIT a two-segment Trace (steps_per_frame=2) with Celeste meta {rx,ry,spawn} per segment.

    python3 test/playtest/celeste/make_solution.py            # derive + verify + write solution.trace.json
    python3 test/playtest/celeste/make_solution.py --shipped  # skip the search, use the twin's shipped route
                                                              # (still VM-verified) — fast path for CI
"""
import sys, os, time, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
PT = os.path.join(HERE, "..")                        # test/playtest (shared: trace.py)
sys.path.insert(0, PT)
sys.path.insert(0, os.path.join(PT, "fake08-sim"))   # fake08sim
sys.path.insert(0, HERE)                             # celeste_solver package
import fake08sim as VM
from trace import Trace, Segment, keys_to_mask
import celeste_solver.solve as TW
import celeste_solver.celeste_sim as S

ROOMS = [(0, 0), (1, 0)]
SPAWN = {(0, 0): [8.0, 96.0], (1, 0): [8.0, 112.0]}
NAME = {(0, 0): "100 M", (1, 0): "200 M"}
CART = os.path.join(os.getcwd(), "assets/celeste.p8")


def vm_clears(rx, ry, masks):
    """Replay `masks` on the exact device VM from room (rx,ry)'s spawn. Return (cleared, frames_used,
    (next_rx,next_ry), min_y). cleared == the room (rx,ry) advanced to another room."""
    VM.spawn(rx, ry)
    start = VM.read(); miny = start['y']
    for i, m in enumerate(masks):
        VM.step_mask(m)
        st = VM.read()
        miny = min(miny, st['y'])
        if (st['rx'], st['ry']) != (rx, ry):
            return True, i + 1, (st['rx'], st['ry']), miny
    return False, len(masks), (start['rx'], start['ry']), miny


def route_to_masks(route):
    """A twin macro-route -> (masks, twin_outcome). masks is the per-game-frame held-button mask list."""
    plan, outcome = TW.plan_from_route(route)     # per-game-frame key strings (freeze frames baked in)
    return [keys_to_mask(k) for k in plan], outcome


def _verify_pick(rx, ry, candidates):
    """VM-verify (candidates) best-first; return (masks[:used], next_room, label) for the first that clears
    room (rx,ry) on the EXACT device VM. `candidates` is a list of (label, masks)."""
    for label, masks in candidates:
        cleared, used, nxt, miny = vm_clears(rx, ry, masks)
        print(f"    {label:<10} -> VM {'CLEAR' if cleared else 'fail '} "
              f"(used {used}/{len(masks)}, VM min_y={miny}, -> room {nxt})", flush=True)
        if cleared:
            return masks[:used], nxt, label
    return None


def derive_room(rx, ry, use_shipped, max_candidates=25):
    """Get a VM-verified per-game-frame mask list for room (rx,ry). The EXACT-VM replay is the acceptance
    gate — the twin only proposes; a route is used only if the real VM clears on it.
    - --shipped: VM-verify the twin's canonical (shipped) route only (fast, no search).
    - default:   re-run the twin beam search, VM-verify its winning routes best-first, and fall back to the
                 twin's canonical route (also VM-verified) if none of the fresh routes survive the VM. This
                 matters for room (1,0): the twin's win-classifier diverges from the VM there, so its fresh
                 'wins' stall on the real VM while its canonical jump/wall-climb route clears."""
    S.set_room(rx, ry)
    shipped_masks, shipped_out = route_to_masks(TW.SHIPPED[(rx, ry)])
    if use_shipped:
        print(f"  canonical route: twin={shipped_out}", flush=True)
        got = _verify_pick(rx, ry, [("shipped", shipped_masks)])
        if not got:
            raise RuntimeError(f"canonical route did not clear room ({rx},{ry}) on the VM")
        return got[0], got[1]

    print(f"  searching room ({rx},{ry}) with the physics twin (beam_solve_fast)...", flush=True)
    t0 = time.monotonic()
    wins = TW.beam_solve_fast(verbose=False)          # genuine offline re-derivation
    print(f"  found {len(wins)} winning routes in {time.monotonic()-t0:.1f}s; VM-verifying...", flush=True)
    cands = []
    for rank, (miny_twin, ndash, nmac, route) in enumerate(wins[:max_candidates]):
        masks, twin_out = route_to_masks(route)
        if twin_out == 'win':
            cands.append((f"fresh#{rank}", masks))
    cands.append(("shipped", shipped_masks))          # twin-canonical fallback (also VM-gated)
    got = _verify_pick(rx, ry, cands)
    if not got:
        raise RuntimeError(f"no route (fresh or canonical) cleared room ({rx},{ry}) on the VM")
    masks, nxt, label = got
    print(f"  accepted: {label}", flush=True)
    return masks, nxt


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--shipped", action="store_true",
                    help="use the twin's shipped route (still VM-verified) instead of re-searching")
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "solution.trace.json"))
    args = ap.parse_args()

    VM.init(CART)
    print(f"cart: {CART}")
    segments = []
    for (rx, ry) in ROOMS:
        print(f"room ({rx},{ry}) = {NAME[(rx,ry)]} (spawn {SPAWN[(rx,ry)]}):")
        masks, nxt = derive_room(rx, ry, args.shipped)
        segments.append(Segment(NAME[(rx, ry)], masks,
                                {"rx": rx, "ry": ry, "spawn": SPAWN[(rx, ry)]}))
        print(f"  -> segment '{NAME[(rx,ry)]}': {len(masks)} game-frames, clears to room {nxt}")

    tr = Trace("celeste.p8", segments, steps_per_frame=2,
               meta={"solver": "celeste/make_solution.py (physics-twin beam search, VM-verified)",
                     "clears": "100 M -> 200 M -> 300 M"})
    tr.save(args.out)
    print(f"\nwrote {args.out}: {len(tr.segments)} segments, {tr.total_frames()} total game-frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
