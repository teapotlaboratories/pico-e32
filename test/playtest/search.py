#!/usr/bin/env python3
"""Generic replay-from-root beam search on the EXACT device VM (test/playtest/fake08-sim) — cart-agnostic.

The engine owns the replay-from-root loop: for every candidate it `reset()`s the VM to the segment start
and replays the prefix, observes the new macro's frames, dedups, and keeps the beam ranked by a score. A
CART supplies the callables (reset / observe / win / dead / score / signature) and the macro set — nothing
here is Celeste-specific. Celeste's adapter is `test/playtest/celeste/solve.py`. See test/playtest/README.md.

This is a LIBRARY (no CLI): a cart's `solve.py` imports `beam_search` and emits a portable Trace.

Perf note: replay-from-root re-plays a node's whole prefix per expansion (beam * |cands| * prefix_len per
depth) — fine for room-sized problems; VM savestates (parked) would make it O(1)/node. Keep the beam modest.
"""
import os, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake08-sim"))
import fake08sim as VM


def beam_search(reset, observe, cand_masks, *, is_win, is_dead, score, signature,
                beam=300, depth=18, extra_after_win=2, verbose=True):
    """Beam over macro-actions on the exact VM. Returns winning routes as (best_score, n_frames, frames),
    where frames = the per-game-frame mask list that clears the segment. All callables are cart-supplied:

      reset()        put the VM at the segment start          (e.g. lambda: VM.spawn(rx, ry))
      observe()      -> a state dict from the VM              (e.g. VM.read)
      is_win(st)     -> bool: this segment is cleared
      is_dead(st)    -> bool: prune this branch
      score(st)      -> number, LOWER is better (the beam keeps the lowest running score)
      signature(st)  -> hashable dedup key

    cand_masks is {name: [mask, ...]} (a macro -> its per-frame held-button masks)."""
    cands = list(cand_masks.items())
    B = [([], float('inf'))]            # (prefix_masks, best_score)
    seen = set(); wins = []; first = None
    for d in range(depth):
        t0 = time.monotonic(); nxt = []
        for prefix, bs in B:
            for _, cm in cands:
                pm = prefix + cm
                out, st, best_new, wl = _eval(reset, observe, is_win, is_dead, score, pm, len(cm))
                s = min(bs, best_new)
                if out == 'win':
                    wins.append((s, wl, pm[:wl])); continue
                if out == 'dead':
                    continue
                key = signature(st)
                if key in seen:
                    continue
                seen.add(key); nxt.append((pm, s))
        nxt.sort(key=lambda z: z[1]); B = nxt[:beam]
        if verbose:
            print(f"  depth {d+1}: beam={len(B)} wins={len(wins)} "
                  f"best_score={B[0][1] if B else float('inf'):.0f}  ({time.monotonic()-t0:.1f}s)", flush=True)
        if wins and first is None:
            first = d
        if not B:
            break
        if first is not None and d >= first + extra_after_win:
            break
    wins.sort(key=lambda w: (w[1], w[0]))       # fewest frames, then best score
    return wins


def _eval(reset, observe, is_win, is_dead, score, prefix_masks, new_len):
    """Replay a prefix from the segment start. The prefix minus its last macro was validated alive at an
    earlier depth and the VM is deterministic, so batch-step it with NO observe (fast) and only observe the
    `new_len` new frames (accurate outcome + score). -> (outcome, state, best_score_over_new, frames_used)."""
    reset()
    n = len(prefix_masks); head = n - new_len
    if head > 0:
        VM.steps(prefix_masks[:head])                       # replay the validated prefix, no observe
    best = float('inf'); st = None
    for j in range(head, n):
        VM.step_mask(prefix_masks[j]); st = observe()
        if is_win(st):
            return 'win', st, best, j + 1
        if is_dead(st):
            return 'dead', st, best, j + 1
        best = min(best, score(st))
    return 'alive', st, best, n
