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

---

# Solving a room CLOSED-LOOP + running it on the board (the workflow)

The trace above is **open-loop** (pre-recorded masks). The richer path is a reactive **`policy(state)->mask`**
per room (like the Pico Racer) that reads the live state each frame and reacts, run on real hardware over the
**fc-scheduled input backend**. Rooms **100 M / 200 M / 300 M** are solved this way (sim + device). **400 M** is
solved too (sim + device) but as a **raw-mask replay** rather than a reactive policy — its route is a
pixel-precise wall-jump chain that doesn't reduce to state predicates; see *"400 M — the raw-mask exception"*
after step 6. The per-room recipe — follow it for the next room:

**1. See the room.** Dump geometry + hazards + objects so you know the layout before authoring anything:
- collision grid: `for ty,tx: fget(mget(rx*16+tx, ty), 1)` → a 16×16 solid map (note: spikes are NOT flag-1).
- objects: iterate `all(objects)`, read `o.spr` (**18=spring, 26=fruit/berry, 17=up-spike tile**) + `o.x/o.y`.
- render: `gym._rgb_to_png(VM.frame_rgb(), path, 4)` after `VM.spawn(rx,ry)`; `gym.run_filmstrip(masks, ...)` for a
  maneuver. Read the PNG by eye.

**2. Get a reference maneuver (what actually clears the room).**
- If an open-loop trace covers it (`solution.trace.json` has 100 M/200 M), decode its per-frame masks — it shows
  the dash/jump/spring sequence and the launch positions.
- Else **search**. The `celeste_solver` twin (`beam_solve_fast`) is fast but models terrain+spikes only, **NOT
  springs** — so its "wins" won't VM-verify for a spring room (they all failed for 300 M). For spring/hazard rooms,
  beam-search on the **REAL VM** (full physics; pattern in a scratch `beam300.py`): small beam (`VM.spawn` is
  ~15 ms and savestates are broken → replay-from-root), fitness = min-y **steered toward the exit column when
  high** (pure min-y stalls below a spike field).
- Replay the reference with the rich obs (`closed_loop.read`'s Lua TAIL) and log the state at each jump/dash →
  those are your launch predicates.

**3. Author a reactive `ClimberNNN`** in `closed_loop.py`: an ordered phase machine where each dash/jump/spring/
wall-jump fires on a **STATE predicate** (grounded / rising into a launch band / dash-available / wall-contact /
just-bounced), never a frame index — robust to drift. Add a `spawned` gate (hold neutral until grounded at the
spawn) so it survives the room-entry transition when chained. Thresholds live in a `PARAMSNNN` dict, tuned on the
sim (grid-search / eyeball the filmstrip).

**4. Register + verify on the sim.** Add `(rx,ry): (name, spawn, ClimberNNN, deathy)` to the `ROOMS` registry.
Verify deterministically: `drive_sim_room((rx,ry))` (isolation) **and** `drive_sim_chain(((0,0),…,(rx,ry)))` (the
whole chain — the board reaches room N only by clearing N-1, so the device always plays from 100 M).

**5. Run it on the board (fc-scheduled).** Firmware (already flashed):
`-D CELESTE=1 -D INPUT_BACKEND=scheduled -D TELEMETRY=1 -D FORCE_FLASH_CART=1 -D SHOW_FPS=1 -D CENTER_GAME=1`
(the driver skips the title with jump commands; no `CELESTE_START`). Two delivery shapes in `fc_device.py`:
- `--predictive` — twin-in-the-loop, rebases on divergence. Clears the forgiving rooms (100 M/200 M).
- `--openloop --to300` / `--to400` — blind fc-scheduled replay of the pre-solved masks. Clears **all** rooms incl.
  the frame-precise 300 M and 400 M (`--to400`: 100→200→300→400 M, CLEAR at fc=1454, 723 game-frames, fps
  7.3/28.4/30.0). **Key finding: on a deterministic board, open-loop > twin-in-the-loop for frame-precise rooms**
  — the rebase re-plans on a dash's transient (>6px) divergence and can't recover a missed pixel-dash, so blind
  replay just works. (Diagnosis: `fc = _picoFrameCount`, exactly 2/game-frame incl. dash freezes; board and twin
  hit each spawn at the same game-frame; a ~1-frame fc offset from the room-transition flash is fatal only to
  300 M under the predictive path.)

**6. Video.** `render_compare.py <name>` (or `all`) records the device open-loop over the bench camera, renders
the sim **synced to the device's per-room spawn times** (the sim freezes at each spawn until the board arrives),
and writes a side-by-side `videos/<name>.mp4`.

### 400 M — the raw-mask exception

Not every room reduces to a reactive `ClimberNNN`. **400 M** (the fall-floor room) is a **pixel-precise
wall-jump chain**, so it ships as a **raw mask list** replayed by `ReplayClimber` (in `closed_loop.py`, same
`spawned` gate as the Climbers). Two VM-verified routes in `routes/` — both climb the right-side fall-floor
staircase (B→E→F→C) with **jumps only** (the dash is saved for the exit), then reach the top:

- `room400_route.json` (143 f) — wall-jump off D, dash onto the "1" block, dash out.
- `room400_altroute_viaD.json` (189 f, **the shipped one**) — wall-jump off D, then **wall-jump off the block
  back onto D** (lands *on* D, matching the hand-drawn route), then onto the block and out.

Three physics facts that room turns on, none obvious from the tile map: fall-floors **crumble the instant you
leave** (the climb is continuous jumps, no dawdling); the top-right block **D is solid from *below*** so a
straight-up dash bonks its underside — you mount it with a **wall-jump off its side wall** (`is_solid` misses
fall-floor *objects*, so the telemetry `wall` flag reads 0 — test the wall-jump empirically, it fires: spy→−2);
and the exit only opens through **cols 5-7**, gated by a solid ceiling over cols 8-15, so the launch *must* come
off the "1" block top. Routes were found by a jump-only landing-beam over the staircase + a placement-seeded
search of the top (scratch tools, throwaway). Registered as `ROOMS[(3,0)]` via `make_replay(ROUTE_400, spawn)`;
verify with `drive_sim_chain(((0,0),(1,0),(2,0),(3,0)))` and run with `--openloop --to400`.

### Closed-loop files

| file | what |
|---|---|
| `closed_loop.py` | reactive policies (`Climber`=100 M, `Climber200`=200 M, `Climber300`=300 M) + **`ReplayClimber`/`make_replay`** (raw-mask rooms, e.g. 400 M) + `PARAMS*` + the `ROOMS` registry + `Chain` + `drive_sim_room` / `drive_sim_chain` + the rich-obs `read` TAIL. |
| `routes/` | VM-verified raw-mask routes for non-reactive rooms (`room400_route.json`, `room400_altroute_viaD.json`). |
| `fc_device.py` | board drivers: `drive_device_chain` (open-loop, `--openloop [--to300|--to400]`) + `predictive` (twin-in-the-loop) + the title-skip `_celeste_warmup` + `_celeste_twin`. |
| `render_compare.py` | the synced SIM \| DEVICE comparison-video tool (`all` / `<name>` / `--sim-only`; `400m` included). |
| `../live.py` | cart-agnostic runners: `drive_sim`, `drive_device`, `drive_device_predictive`. |
| `../pico_racer/` | the racer — the other genre (continuous), where LIVE closed-loop + rebase *do* work. |

Full narrative + the fc/freeze/game-frame diagnosis: `docs/worklog/2026-07-20-celeste-closed-loop-fc-scheduled.md`.
