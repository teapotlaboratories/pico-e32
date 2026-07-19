# Celeste — solved, verified `Trace` (the solver-agent deliverable)

This directory is a **standalone, isolated** solution package for the PICO-8 cart **Celeste (Classic)** on
the fake-08 port. Everything here was produced with the shared gym (`../fake08-sim`, `../gym.py`,
`../trace.py`) imported **read-only**; nothing outside `test/playtest/celeste/` was modified.

## What the cart is

Celeste Classic (Matt Thorson & Noel Berry), the PICO-8 mountain-climb platformer. Booting it in the gym and
reading the rendered frames (title -> "CELESTE" + "X+C", then the rooms) confirms it: a vertically-stacked
climb where the player (red-haired girl) starts at the **bottom** of a room and must reach the **top** to
advance. Buttons: L/R move, **O = jump (z/o)**, **X = dash (x)**. A room is *cleared* when the player exits
the top — in VM terms the room index **(rx,ry) changes** (rooms are an 8-wide grid; exiting room *n* at the
top advances to *n+1*, so (0,0)->(1,0)->(2,0)).

The deliverable clears the first two rooms in order:

| segment | room (rx,ry) | spawn | shown as | clears to |
|---|---|---|---|---|
| `100 M` | (0,0) | (8, 96)  | "100 M" | room (1,0) = "200 M" |
| `200 M` | (1,0) | (8, 112) | "200 M" | room (2,0) = "300 M" |

## How I inspected it (with the gym — the eyes)

Booted `assets/celeste.p8` via `fake08sim`, and used `gym.snapshot()` to render and **Read** the title
screen and each room's spawn frame. `VM.spawn(rx,ry)` puts the player at a room's spawn; `VM.read()` returns
`{x,y,rx,ry,dj,fz,...}`, and **room progress = (rx,ry) changing** (confirmed by eye against the on-screen
"100 M"/"200 M"/"300 M" labels). This is the universal, cart-agnostic observation: I looked at the screen.

## How I obtained each room's input

I used the shipped offline **physics twin** (`celeste_solver/celeste_sim.py` + `celeste_solver/solve.py`) as
a legitimate solver tool. `make_solution.py` drives it:

1. **Derive** — re-run the twin's `beam_solve_fast` beam search from scratch (genuinely searched, not copied)
   to enumerate winning macro-routes for the room. The twin models Celeste's *move-before-update* ordering
   and the *2-frame dash freeze*, so its per-game-frame plan is indexed to the VM frame counter.
2. **Convert** — `plan_from_route` -> a per-game-frame key plan (freeze frames baked in) -> held-button masks.
3. **VERIFY on the exact VM (the acceptance gate)** — `VM.spawn(rx,ry)`, step the masks, and *require the
   room to advance*. Fresh routes are tried best-first; the first that clears **on the real VM** is accepted,
   with the twin's canonical route as a VM-gated fallback. A route is used **only if the device's own VM
   clears on it** — the twin merely proposes.

### The twin is a model; the VM is ground truth (the interesting finding)

The acceptance gate did real work — the twin diverges from the VM, in both directions:

- **Room (0,0):** the fresh search's top route (`fresh#0`) *also* "won" in the twin, but **failed on the VM**
  (stalled at `min_y=-3`, one pixel short of the `y<-4` exit). `fresh#1` cleared on the VM (94 frames,
  `min_y=-4`). So the fresh, VM-verified route is the accepted one — the twin alone would have shipped a
  route that doesn't clear.
- **Room (1,0):** **all 25** fresh twin-"win" routes stalled on the VM at `min_y=63..78` (barely halfway up).
  Their dash-heavy shortcuts exploit twin/VM sub-pixel divergence in this room. The twin's **canonical
  jump/wall-climb route** clears on the VM (113 frames, `min_y=-3` -> room (2,0)) — and, tellingly, the twin's
  own `plan_from_route` even classifies that route as `alive` (*not* a win), i.e. the twin under-predicts it.
  The VM says otherwise, so the VM wins. This is why room (1,0)'s segment uses the canonical route: it is the
  one the **real VM** clears.

So the final trace = **fresh-derived room (0,0)** + **VM-verified canonical room (1,0)**, both gated on the
exact device VM.

## How I verified (SEE it, don't assert it)

`verify_solution.py` loads `solution.trace.json` and, per segment:

1. **Sim replay:** `spawn(rx,ry)`, step the segment's masks, assert `(rx,ry)` advances. Result:
   - `100 M`  room (0,0): **CLEAR**, 94/94 frames, climbed to `min_y=-4`, room (0,0) -> **(1,0)**.
   - `200 M`  room (1,0): **CLEAR**, 113/113 frames, climbed to `min_y=-3`, room (1,0) -> **(2,0)**.
2. **Look:** `gym.run_filmstrip(...)` montages sampled, labelled frames (`f<i> y<y> r<rx>,<ry>`) into one PNG
   per room. Read by eye: in both rooms the player visibly traverses from the bottom-left spawn up and out the
   top, and the labelled `y` decreases monotonically to the exit (100 M: y96->-3; 200 M: y112->4 then exit).

Because the fake-08 VM here **is** the device's VM (same code + deterministic + same inputs), a trace that
clears on the sim clears on the device from the same file (delivered frame-synced by `../harness.py` /
`celeste_playtest.py --trace=solution.trace.json`).

## Files

| file | what |
|---|---|
| `solution.trace.json` | **the deliverable** — the verified replay-able `Trace`: 2 segments (`100 M`, `200 M`), `steps_per_frame=2`, 207 game-frames, Celeste meta `{rx,ry,spawn}` per segment. |
| `make_solution.py` | re-runnable producer: twin beam search -> convert -> **VM-verify** -> write the trace. `--shipped` skips the search and VM-verifies the twin's canonical routes (seconds, for CI). |
| `verify_solution.py` | independent verifier: sim-replay both segments (assert clear) + render the filmstrips. |
| `celeste_solver/` | the shipped offline physics twin + beam search used to derive the routes (unmodified). |

## Reproduce

```sh
# from the repo root; needs the sim built: make -C test/playtest/fake08-sim
python3 test/playtest/celeste/make_solution.py            # genuine search + VM-verify -> solution.trace.json (~6 min)
python3 test/playtest/celeste/make_solution.py --shipped  # fast VM-verified path (no search, seconds)
python3 test/playtest/celeste/verify_solution.py <dir>    # sim replay both segments + write filmstrips to <dir>
```
