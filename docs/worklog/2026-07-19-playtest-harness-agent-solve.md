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

## State at end of session

- **Done:** M0 (reorg), M1 (replay-from-root), M2 (Trace + dual replay + frame-count sync), M3 (search as a
  tool). **Next:** M4 (agent gym), M5 (spawned solver on Celeste). Then M6 fps (unify `MEASURE_FPS` +
  `TELEMETRY`, achieved + headroom), M7 video, M8 orchestrator, M9 a non-platformer cart.
- **Board:** left flashed with the play-test build (`CELESTE + INPUT_BACKEND=serial + INPUT_HOLD_FRAMES=1 +
  FORCE_FLASH_CART + SHOW_FPS + TELEMETRY + CENTER_GAME`), idle at room 2 — a known-good dev build.
- **Fork:** the eris experiment is reverted; `components/fake08/fake08` is clean (no gitlink change).
- **Verification:** host-side (sim build, frame-count arithmetic, dual replay) verified by unit-style runs on
  the desktop VM; the device half of the trace replay verified on real hardware (PASS 2/2). The eris dead-end
  verified via ASAN.
