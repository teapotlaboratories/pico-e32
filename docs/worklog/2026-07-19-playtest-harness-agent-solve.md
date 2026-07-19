# 2026-07-19 — Play-test harness → a gym for agent-solved carts + FPS measurement

Goal: turn the one-off Celeste play-test into a reusable rig whose ultimate purpose is **measuring real
on-device FPS across full playthroughs of arbitrary carts**, with the playthrough produced by an **agentic
AI (no human)**. This log covers the restructure, the replay-able-`Trace` contract, the host↔device
frame-count sync, a savestate dead-end (parked), and the pivot from "a fixed search algorithm" to "a gym an
agent solves in."

Area doc / milestones: [`test/playtest/README.md`](../../test/playtest/README.md). This log is the running
record; the README is the plan.

## 1. Restructure into `test/playtest/` (M0)

Moved (via `git mv`, history preserved) the Celeste play-test tooling out of `tools/` into a per-cart layout:

```
test/playtest/
├─ trace.py           the solution contract (per-frame masks in segments)
├─ harness.py         generic device driver (frame-synced serial delivery + verification)
├─ fake08-sim/        the SHARED native VM (cart-agnostic) — moved from tools/fake08-sim
└─ celeste/           per-cart area: celeste_playtest.py, celeste_solver/, render_run.py
```

The shared VM sits at the top level (any cart reuses it); only cart-specific tooling lives under `<cart>/`.
Fixed the path depths on the move (the sim `Makefile` `../../` → `../../../` to reach the repo root now that
it's one dir deeper; `celeste_sim.py`'s cart path likewise), and updated ~6 external references
(`firmware/.../main.cpp` comment, `tools/record_video.sh`, four docs). `render_run.py` (Celeste-specific)
moved under `celeste/`; `fake08sim.py` (generic) stayed with the shared VM.

## 2. Replay-from-root backend (M1)

Validated the primitive a generic solver needs: `spawn(rx,ry)` + `step` on the exact VM.
- **Deterministic:** replaying the same input from spawn gives byte-identical final state across runs.
- **Reproduces the clear:** replaying the known Celeste room-0 plan reaches room `(1,0)`.

So a search can reach any node by `spawn + replay(prefix)` — no savestate needed. Command:
`python3` driving `fake08sim`; final state `{x:103, y:-4, rx:1, ry:0}` (crossed the top exit), identical both runs.

## 3. The `Trace` contract + dual replay (M2) — the hard requirement

A **solution is a portable, replay-able `Trace`** (per-game-frame button masks in ordered segments), not a
savestate or a solver object. Hard invariant: **it must replay-to-clear on BOTH the sim and the device**,
from the same file.

Built `test/playtest/trace.py` (format + `keys↔mask`), added `fake08sim.step_mask()` (replay a raw mask),
and made `celeste_playtest.py` accept `--trace=<file>`. Proof — the **same** `celeste_solution.trace.json`
(2 segments, 210 frames):
- **sim replay:** `100 M → room(1,0)`, `200 M → room(2,0)` — PASS.
- **device replay:** `CLEARED 100 M → 200 M → 300 M`, PASS 2/2, exit 0 (board on `/dev/ttyUSB0`, CP2104).

Because the sim *is* the device's VM, a trace that clears on the sim clears on the device.

## 4. Host↔device frame-count sync (M2)

To let a host-built solution line up with the device's frames, both sides must share the VM's frame clock.
The device telemetry already streams it (`T <frame> …` from `vm->GetFrameCount()`); the **host did not
expose it**. Added `sim_frame_count()` → `vm->GetFrameCount()` (`test/playtest/fake08-sim/sim.cpp`), surfaced
as `fake08sim.frame_count()` and included in every `read()['fc']`.

Verified the two are the same clock: the host counter **increments +2 per game-frame** (a 30 fps cart = 2 VM
`Step()`s/frame), matching the device's per-Step count — replaying 97 game-frames advanced it by exactly
**194**. The absolute origins differ (host was 116 at spawn; device differs by its own boot sequence), so the
harness anchors the host's *relative* frames to the device telemetry count at each segment start (`plan_fc0`):
frame count = the clock, the anchor = the phase.

## 5. eris VM savestates — investigated, then PARKED (dead-end, honest)

O(1) snapshot/restore would make a *search* fast, so I investigated why the vendored eris savestate returns 0.

**Root cause (confirmed):** `Vm::serializeLuaState` (`components/fake08/fake08/source/vm.cpp:1493`) pcalls
`eris.persist_all`, which errors because `eris.init_persist_all()` — which builds the perms table by walking
sorted `_G` — is **commented out** (`vm.cpp:170-184`). With an empty perms table, eris tries to serialize the
~120 registered C-API functions and raises `ERIS_ERR_CFUNC`. The bios already *defines*
`init_persist_all`/`persist_all`/`restore_all` (`p8GlobalLuaFunctions.h:337-379`); eris is already linked in
both sim and device builds.

**Experiment:** uncommented the block in the fork working tree, rebuilt the sim, and ran a round-trip
acceptance test (save at spawn → replay the plan → restore → replay → assert identical state + framebuffer).
- A **single round-trip is bit-exact** — `serializeLuaState` produced a **152 KB** blob and restore
  reproduced the state.
- **But under load it corrupts.** ASAN (built via a new `EXTRA_CXXFLAGS`/`EXTRA_LDFLAGS` hook in the sim
  `Makefile`) pinned two distinct bugs: a **heap-use-after-free in `u_proto`** during unpersist (GC frees a
  half-rebuilt `Proto`; a `collectgarbage("stop")` guard reduced but did not eliminate it), and then a
  **global-buffer-overflow in `Vm::Step`** after restore (`ldo.c:319`). Flaky (~2/4 runs under ASAN).

This is exactly why upstream commit `47c48ad "debug segfault in lua close or load cart"` disabled it. Making
eris robust for this z8lua/fix32 build is an open-ended multi-bug fix. **Decision: revert the fork edits (fork
clean again) and PARK it.** The round-trip test + the ASAN `Makefile` hook are left in place for a future pass.
It is **off the critical path** — replay-from-root supersedes it (§2, §6).

## 6. Search engine (`search.py`) — built, then demoted to a tool

Built a generic replay-from-root beam search (reusing the twin's proven macro set; dedup on the VM's
`read()` signature, which already matches the twin's winning key `(x,y,djump,freeze,sign spd)` — so no state
widening was needed). It **climbs room 0 deterministically** (min-y 96→69→58→53).

**Profiling finding (flipped my assumption):** `step` = **242 µs**, `read` = **47 µs** — the cost is VM
*stepping* (each frame runs Celeste's real `_update`+`_draw`), not reading. Replay-from-root re-steps the
~27-frame spawn animation **plus the whole prefix per eval**, so a narrow beam (80) stalls (can't hold the
lateral-setup states the solution needs) and a wide beam is slow (~30–50 min/room). Batching the prefix
without reads barely helped (reads were already cheap).

Conclusion: replay-from-root is *correct* but VM-step-bound. Rather than chase wide-beam perf (or the parked
savestates), the search is **demoted to an optional per-genre tool** an agent may invoke — not the path.

## 7. Pivot: the harness is a *gym*, an agent is the solver

Key correction from the owner: **other carts are not guaranteed to be Celeste-shaped** (rooms, a player
object, a climb heuristic, a solvable-by-fixed-input traversal), and **no human** should record the solution.
So the genericity cannot live in a fixed solver — it lives in the *environment*:

- Build a deterministic, fully-observable **gym**; a **spawned solver agent** (one per cart) drives it.
- The agent **observes via rendered frames** (the universal channel), and **writes its own per-cart
  instrumentation** by reading the cart's `.p8` Lua (the way Celeste's `READ_LUA` was hand-written).
- **Isolation contract:** the agent may use tools and write whatever scripts it needs, but its entire output
  is **standalone and isolated in `test/playtest/<cart>/`**; the shared core (`fake08sim`, `trace`, `harness`,
  `search`) is imported read-only. Deliverable = a self-contained solution package (`solve.py`,
  `solution.trace.json`, helpers, notes).
- Algorithmic solvers (the §6 beam search) are optional tools the agent may call.

Next: **M4** — the gym's "eyes" (frame + filmstrip rendering, a `run(inputs)→observe` primitive); **M5** —
spawn a Celeste solver agent against it and check its trace against the known-good reference.

## 8. Review + M4 — the gym's eyes

Opened PR #13 for the arc above and self-reviewed it with the local `/review`: hardened `search.py`'s CLI to
`argparse`, added a `trace.py` format-version guard, added host-side tests (`test_trace.py` 4/4,
`test_sim_smoke.py` 1/1), and made `search.py` genuinely cart-agnostic — the engine now takes callables
(`reset/observe/is_win/is_dead/score/signature`) and the Celeste adapter moved to the conventional
`celeste/solve.py` (behaviour preserved: same room-0 climb 96→53).

**M4 (done) — the gym's eyes.** A solver agent solves by *looking* at the screen, so `gym.py` gives it one:
`snapshot(path)` renders the current VM frame to a PNG; `run_filmstrip(masks, path, every=/reset=/label=)`
replays an input run and montages sampled, labelled frames into one contact-sheet PNG the agent Reads in a
single look (and returns the captured `read()` states). Added `fake08sim.draw()` (binds `sim_draw`). Verified
on Celeste room 0 — the filmstrip shows the whole run at a glance, the climb reading **y96 → y4** across the
tiles, so the agent perceives motion/progress, not just a still.

## 9. M5 — a spawned agent solves Celeste through the gym

The payoff. Spawned a **Celeste solver agent** scoped to `test/playtest/celeste/` with the gym as its toolset
(`fake08sim`, `gym.py`, `trace.py`, the twin), and gave it the isolation contract + the **Collaborate**
affordance (it may share a `gym` image and ask the owner a question mid-solve — routed via `SendMessage` to
`main`). It:

- **Inspected** the cart with `gym.snapshot` — booted Celeste, Read the title + room frames, and confirmed
  the win condition (room `(rx,ry)` advances) by eye against the on-screen "100 M"/"200 M"/"300 M" labels.
- **Derived** each room's input by re-running the twin's `beam_solve_fast`, then **VM-gated every route on the
  exact device VM** — a route is accepted only if the *real VM* clears on it.
- **Emitted** `celeste/solution.trace.json` (2 segments, **207 game-frames**: 100 M = 94, 200 M = 113), plus
  `make_solution.py` (re-runnable producer), `verify_solution.py`, and `README.md` — all **isolated in
  `celeste/`, shared core untouched** (0 files modified outside).

**The finding that made the gate matter:** the twin is a *model*, the VM is *ground truth*. For room (0,0)
the twin's top route also "won" in the twin but **failed on the VM one pixel short**; the next candidate
cleared (a fresh 94-frame route, distinct from the 97-frame reference). For room (1,0), **all 25** fresh
twin-"win" shortcuts stalled on the VM at ~halfway (min_y 63–78 — the dash-heavy routes diverge there), while
the twin's canonical wall-climb route — which the twin even mislabels as *not* a win — is the one the real VM
clears. So without the VM-gate the agent would have shipped broken traces.

**Independently verified (not the agent's word):** my own sim replay clears both rooms (94/94, 113/113); a
`gym.run_filmstrip` of room 0's fresh route, Read by eye, climbs y96 → −3 and exits the top; and **device
replay of the agent's `solution.trace.json` on the real board cleared 2/2** (`CLEARED 100 M → 200 M → 300 M`,
t≈12 s / 17 s). Committed to PR #13 (`1c189b6`).

## 10. M6 — a play-test measures its own fps

Unified the standalone `MEASURE_FPS` timing into the `TELEMETRY` stream, so a play-test measures its frame
rate on the board with no separate build. The `TELEMETRY` loop
(`firmware/pico-e32-fake08/main/main.cpp`) now times `vm->Step()` and `host->drawFrame()` and streams
`T <frame> <step_us> <draw_us> <player-state…>` — the `<frame> <step> <draw>` prefix is generic; the
`ExecuteLua` telemetry poke is *not* timed, so the numbers are the cart's real per-frame compute.

`harness.FpsMeter` groups the per-Step timing into game-frames (`steps_per_frame`) and reports, given the
target rate, min/avg/max of **achieved** = `min(target, ceiling)` (does the device hold the rate?) and
**headroom** = `1e6/compute` (the uncapped ceiling). Achieved is derived from *compute vs the frame budget*,
not wall-clock — so the per-frame telemetry overhead (the ExecuteLua + serial TX) doesn't skew it.
`harness.run(fps_out=…)` returns the stats. `celeste_playtest.parse_line` was updated for the new format;
`test_fps.py` unit-tests the aggregation.

**Measured on the board while clearing Celeste** (still PASS 2/2, so input-sync is unaffected): over 510
game-frames (target 30) — **achieved 9.2 / 29.9 / 30.0**, **headroom 9.2 / 64.1 / 112.6**. It holds 30 fps
(avg 29.9) with ~2× compute headroom, and a single room-load spike (~108 ms) dips it to 9.2 — consistent with
Gate #2's object-count-driven per-room 5–40 ms.

## 11. M7 — generalize the video (sim + device)

Extracted a shared, cart-agnostic sim-video renderer `render.py`: it plays a `Trace` on the sim (a cart's
`reset(seg)` to reach each segment's start, then the segment's masks, with an optional
`stop_on_clear(state, seg)` so the clip ends at the clear), captures every game-frame, and encodes to mp4 at
the game rate (60/`steps_per_frame`). `celeste/render_run.py` is now a thin adapter over it
(reset = `spawn(rx,ry)`, clear = `(rx,ry)` advances) and renders a *trace file* (default
`solution.trace.json`) rather than the embedded plans. The **device** side was already generic —
`tools/record_video.sh -- <a celeste_playtest.py --trace=... run>` (bench camera) records whatever plays.

Verified: rendered the M5 `solution.trace.json` to mp4 (**257 frames / 8.6 s**) via the generalized path, and
filmed the same trace on the board earlier (both sent as side-by-side proof). So one trace → a sim video and
a device video, any cart that supplies a reset.

## State at end of session

- **Done:** M0 (reorg), M1 (replay-from-root), M2 (Trace + dual replay + frame-count sync), M3 (search as a
  cart-agnostic engine + `celeste/solve.py`), M4 (agent gym — the eyes, `gym.py`), M5 (spawned solver agent
  solved Celeste through the gym, verified on the board), M6 (fps unified into the telemetry stream —
  achieved + headroom, measured on the board), **M7 (generalized sim/device video — shared `render.py` +
  Celeste adapter)**. All on PR #13 (branch `playtest-agent-gym`, self-reviewed via `/review`). **Next:** M8
  orchestrator (one call: spawn solver → sim + device → fold fps + both videos into one report), M9 a
  non-platformer cart (proves genre-agnosticism).
- **Board:** left flashed with the play-test build (`CELESTE + INPUT_BACKEND=serial + INPUT_HOLD_FRAMES=1 +
  FORCE_FLASH_CART + SHOW_FPS + TELEMETRY + CENTER_GAME`), idle at room 2 — a known-good dev build.
- **Fork:** the eris experiment is reverted; `components/fake08/fake08` is clean (no gitlink change).
- **Verification:** host-side (sim build, frame-count arithmetic, dual replay) verified by unit-style runs on
  the desktop VM; the device half of the trace replay verified on real hardware (PASS 2/2). The eris dead-end
  verified via ASAN.
