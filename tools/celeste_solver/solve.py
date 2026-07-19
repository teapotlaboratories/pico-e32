#!/usr/bin/env python3
"""Solve a hands-free input sequence that clears Celeste room 1, offline, against the physics twin.

Beam-searches over macro-actions (run / jump / dash-in-8-directions), keeps winning routes ranked by
margin (how far past the exit) and simplicity (fewest dashes), then prints the chosen route AND the
per-game-frame key PLAN to embed in tools/celeste_playtest.py. The twin (celeste_sim.py) models
Celeste's move-before-update ordering and the 2-frame dash freeze, so game-frame index == the firmware
frame counter / 2 and the PLAN can be delivered frame-synced. See the worklog for the method.

    python3 tools/celeste_solver/solve.py            # search + print route + PLAN
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import celeste_sim as S

DASH = {'UR': 'RU', 'U': 'U', 'R': 'R', 'DR': 'RD', 'UL': 'LU', 'L': 'L'}

def macro_frames(kind, arg):
    """A macro -> per-active-frame button-strings (L/R/U/D/O/X)."""
    if kind == 'rest':  return [''] * arg
    if kind == 'right': return ['R'] * arg
    if kind == 'left':  return ['L'] * arg
    if kind == 'jump':  return [arg + 'O', arg, arg, arg]
    if kind == 'dash':  keys = DASH[arg]; return [keys + 'X', keys, keys, keys, keys]
    return ['']

CANDS = ([('right', k) for k in (4, 8, 14)] + [('left', k) for k in (4, 10)] +
         [('jump', d) for d in ('', 'R', 'L')] +
         [('dash', d) for d in ('UR', 'U', 'R', 'UL', 'DR')] + [('rest', 4)])

def route_seq(genes):
    frames = []
    for k, a in genes:
        frames += macro_frames(k, a)
    return [dict((c, True) for c in f) for f in frames]

def apply_macro(player_state_genes, kind, arg):
    """Run the whole route+macro from scratch (the twin is cheap) -> (state after, min y, dead, win)."""
    genes = player_state_genes + [(kind, arg)]
    traj, outcome = S.run(route_seq(genes), 240)
    return genes, min(e[2] for e in traj), outcome, (outcome == 'win')

def beam_solve(BEAM=4000, DEPTH=18, extra_after_win=2, verbose=False):
    beam = [([], 999.0)]        # (genes, best_min_y)
    seen = set()
    wins = []
    first_win = None
    for depth in range(DEPTH):
        nxt = []
        for genes, bm in beam:
            for kind, arg in CANDS:
                g2, miny, outcome, win = apply_macro(genes, kind, arg)
                if win:
                    ndash = sum(1 for k, _ in g2 if k == 'dash')
                    wins.append((miny, ndash, len(g2), g2))
                    continue
                if outcome != 'alive':
                    continue
                # coarse dedup on the resulting state
                traj, _ = S.run(route_seq(g2), 240)
                x, y = traj[-1][1], traj[-1][2]
                key = (int(x) // 3, int(y) // 3, round(min(bm, miny)))
                if key in seen:
                    continue
                seen.add(key)
                nxt.append((g2, min(bm, miny)))
        nxt.sort(key=lambda s: s[1])
        beam = nxt[:BEAM]
        if verbose:
            best = beam[0][1] if beam else 999
            print(f"  depth {depth+1}: beam={len(beam)} wins={len(wins)} best_min_y={best:.0f}", flush=True)
        if wins and first_win is None:
            first_win = depth
        if not beam:
            break
        if first_win is not None and depth >= first_win + extra_after_win:
            break        # a couple of depths past the first win = enough margin variety
    wins.sort(key=lambda w: (w[1], w[0], w[2]))    # fewest dashes, then margin, then macros
    return wins

def n_dash(genes):
    return sum(1 for k, _ in genes if k == 'dash')

def run_macro_inc(p, freeze, active_frames):
    """Step a cloned player through one macro's active frames, modelling freeze (matches celeste_sim.simulate).
    Returns (freeze, outcome, min_y). Fast: only this macro's frames are stepped; state carries between macros."""
    miny = p.y
    ai = 0
    while ai < len(active_frames):
        if freeze > 0:
            freeze -= 1
            miny = min(miny, p.y)
            continue
        btn = active_frames[ai]; ai += 1
        p.move(p.spd_x, p.spd_y)
        if S.spikes_at(p.x + S.HB['x'], p.y + S.HB['y'], S.HB['w'], S.HB['h'], p.spd_x, p.spd_y):
            return freeze, 'dead', miny
        if p.y > 128:
            return freeze, 'dead', miny
        p.dashed_this_frame = False
        p.update(btn)
        miny = min(miny, p.y)
        if p.y < -4:
            return freeze, 'win', miny
        if p.dashed_this_frame:
            freeze = 2
    return freeze, 'alive', miny

def beam_solve_fast(BEAM=8000, DEPTH=20, extra_after_win=2, verbose=False):
    """Incremental beam (carries player+freeze state per node) with full-state dedup — much faster than
    beam_solve and doesn't prune the djump/velocity a final exit dash needs."""
    start = S.Player()
    beam = [(start, 0, [], start.y)]        # (player, freeze, genes, best_min_y)
    seen = set(); wins = []; first_win = None
    frames_cache = {c: [dict((ch, True) for ch in f) for f in macro_frames(*c)] for c in CANDS}
    for depth in range(DEPTH):
        nxt = []
        for p, fz, genes, bm in beam:
            for kind, arg in CANDS:
                q = p.clone()
                fz2, outcome, miny = run_macro_inc(q, fz, frames_cache[(kind, arg)])
                g2 = genes + [(kind, arg)]
                m = min(bm, miny)
                if outcome == 'win':
                    wins.append((m, n_dash(g2), len(g2), g2)); continue
                if outcome == 'dead':
                    continue
                k = (int(q.x) // 2, int(q.y) // 2, q.djump, fz2, S.sign(q.spd_x), S.sign(q.spd_y))
                if k in seen:
                    continue
                seen.add(k)
                nxt.append((q, fz2, g2, m))
        nxt.sort(key=lambda s: s[3])
        beam = nxt[:BEAM]
        if verbose:
            print(f"  depth {depth+1}: beam={len(beam)} wins={len(wins)} "
                  f"best_min_y={beam[0][3] if beam else 999:.0f}", flush=True)
        if wins and first_win is None:
            first_win = depth
        if not beam:
            break
        if first_win is not None and depth >= first_win + extra_after_win:
            break
    wins.sort(key=lambda w: (w[1], w[0], w[2]))
    return wins

def plan_from_route(route):
    frames = []
    for k, a in route:
        frames += macro_frames(k, a)
    seq = [dict((c, True) for c in f) for f in frames]
    _, outcome, game_plan = S.simulate(seq, 240)
    KEY = {'L': 'l', 'R': 'r', 'U': 'u', 'D': 'd', 'O': 'z', 'X': 'x'}
    plan = [''.join(KEY[b] for b in btn) for btn in game_plan]
    while plan and plan[-1] == '':
        plan.pop()
    return plan, outcome

# The routes shipped in tools/celeste_playtest.py, one per room (found by this search; re-derivable by
# running it — `beam_solve_fast` for room (1,0) needs the full-state dedup, plain `beam_solve` suffices
# for room (0,0)). Keyed by room (rx,ry).
SHIPPED = {
    (0, 0): [('dash', 'UR'), ('right', 14), ('dash', 'UR'), ('right', 14), ('jump', ''),
             ('left', 10), ('dash', 'U'), ('right', 14), ('jump', ''), ('right', 8),
             ('jump', ''), ('dash', 'U')],
    (1, 0): [('right', 14), ('jump', ''), ('right', 8), ('jump', ''), ('jump', ''), ('jump', ''),
             ('right', 4), ('jump', ''), ('right', 14), ('dash', 'UR'), ('right', 14), ('jump', ''),
             ('right', 14), ('jump', ''), ('jump', ''), ('right', 4), ('jump', 'R')],
}

if __name__ == "__main__":
    for (rx, ry), route in SHIPPED.items():
        S.set_room(rx, ry)
        plan, outcome = plan_from_route(route)
        print(f"room ({rx},{ry}) shipped route -> {outcome}, {len(plan)} game-frames")
        print("  PLAN =", plan)
    if "--search" in sys.argv:
        rx, ry = (int(sys.argv[-2]), int(sys.argv[-1])) if len(sys.argv) >= 4 else (0, 0)
        S.set_room(rx, ry)
        print(f"\nsearching room ({rx},{ry}) (a couple of minutes)...")
        wins = beam_solve_fast(verbose=True)
        print(f"found {len(wins)} winning routes; best few:")
        for miny, nd, L, g in wins[:5]:
            print(f"  dashes={nd} margin_y={miny:.0f} macros={L}: {g}")
