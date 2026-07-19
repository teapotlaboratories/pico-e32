# Celeste level solver

Offline solver that produces the hands-free input sequences used by
[`celeste_playtest.py`](../celeste_playtest.py) to clear **Celeste rooms 1 and 2
("100 M" → "200 M" → "300 M")** on the fake-08 port. Each room needs several frame-precise dashes/jumps,
so the sequences are **solved**, not hand-tuned, then delivered frame-synced. Method and evidence:
[`docs/worklog/2026-07-18-celeste-playtest-clear.md`](../../../../docs/worklog/2026-07-18-celeste-playtest-clear.md).

## Files

| file | what |
|------|------|
| `celeste_sim.py` | A faithful re-implementation ("twin") of Celeste's player physics + a room's collision, decoded straight from `assets/celeste.p8` (`__map__`/`__gff__`). `set_room(rx,ry)` points it at any room (auto-detects the spawn from tile 1, the `fake_wall` from tile 64). Models `move()` sub-pixel accumulation, jump/gravity/wall-slide, the dash (all 8 directions), **move-before-update ordering**, and the **2-frame dash `freeze`**. Also models the serial input backend. Only static terrain + spikes + `fake_wall` are modelled. |
| `solve.py` | Beam-search over macro-actions (run / jump / dash-8-way). `beam_solve` is the simple version; `beam_solve_fast` is incremental with a **full-state dedup** (position + `djump` + freeze + velocity) — needed for room 2, whose top-gap exit stalls a position-only dedup at y=13. Exports the per-game-frame key `PLAN`. |

## Reproduce the shipped plans

```sh
python3 solve.py            # prints both PLANs embedded in celeste_playtest.py
python3 solve.py --search 1 0   # re-run the search for room (1,0) (a couple of minutes)
```

The printed `PLAN`s are byte-for-byte the `PLAN_100M` / `PLAN_200M` in `../celeste_playtest.py`.

## Why a twin + why it must match frame-for-frame

The clear is delivered **open-loop but frame-synced**: the firmware streams a frame counter + the
player position (`TELEMETRY=1`), and the driver sends each frame's buttons locked to that counter with
`INPUT_HOLD_FRAMES=1` (frame-exact serial). For that to land the dashes, the twin's *timing* — not just
its positions — has to match the real game. Two things make that true and are easy to get wrong:

- **`obj.move(spd)` runs *before* `obj.type.update()`** in Celeste's object loop, so velocity set on
  frame *f* first moves the player on frame *f+1*.
- **A dash sets `freeze=2`**, so the two frames after a dash are fully skipped (`_update` returns early).

The twin was validated against on-device telemetry: standing-jump apex, run-stop position, dash launch
velocity, and the post-dash freeze all match to the pixel/frame. See the worklog. With the twin correct,
the solved plan clears the room **deterministically** — same clear at the same frame, every run.

## Re-using it

To solve a further room, `S.set_room(rx, ry)` then run the search (the exit condition — `y < -4` at a top
gap — is generic across the vertically-stacked rooms). Objects beyond static terrain + spikes + `fake_wall`
are not modelled, so rooms with springs, moving platforms, balloons, etc. would need those added to the
twin first. On hardware, the driver must **drain** the telemetry each loop (act on the newest frame counter)
or delivery lags on later rooms — see the worklog.
