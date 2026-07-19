# 2026-07-18 — Driving Celeste through full levels, hands-free over serial

Goal: extend `test/playtest/celeste/celeste_playtest.py` to clear **full Celeste levels** with no human on the controls,
and leave it as a repeatable, self-checking integration test for the whole fake-08 stack (cart → VM →
input → physics → the 30/60 fps loop). Prior state: Celeste plays end-to-end and the serial input backend
(IN-1) is HITL-verified; the play-test was a framework with a stub timeline.

**Status:** ✅ **done — two levels cleared.** The test clears room 1 ("100 M" → "200 M") **and** room 2
("200 M" → "300 M") **deterministically** (identical clears at t≈12.05 s and t≈16.98 s, every run) and
self-verifies over the wire. Camera confirms **"200 M"** and **"300 M"** on the panel. The bulk of this log
is room 1 (where the method was built); the room-2 extension + its one bug are at the end.

---

## Bench state

| | |
|---|---|
| board under test | `/dev/ttyUSB0` this session — CP2104 (identify by chip, `udevadm`; the `ttyUSB*` number is not stable) |
| bench camera | `/dev/ttyUSB1` — FTDI, `http://192.168.7.135` |
| final flashed build | `pico-e32-fake08`, `DEFS='-D CELESTE=1 -D INPUT_BACKEND=serial -D INPUT_HOLD_FRAMES=1 -D FORCE_FLASH_CART=1 -D SHOW_FPS=1 -D TELEMETRY=1'` |

## First: a real input-mapping bug

The stub timeline (and the memory note) used `x` for "jump". But Celeste is `k_jump=4=O` and `k_dash=5=X`,
and the serial backend maps `z`/`o`→O, `x`→X. So **`x` was dashing, not jumping** — `z`/`o` is jump.
Fixed. (Cited: `assets/celeste.p8:29-30`, `components/input/input_serial.c` key map.)

## The room is genuinely hard — open-loop can't clear it reliably

Decoded room (0,0) straight from the cart (`__map__`/`__gff__`): spawn at (8, 96), floor + up-pointing
spikes along the bottom, exit is the **top gap at cols 13–14** (`player.update`: `if this.y<-4 … next_room()`).
Four camera-driven exploratory drives (v1–v4) confirmed the game starts, runs, jumps, and dashes, and that
**the dash is the only tool that climbs** — but naïve inputs walk her into spikes (the room title kept
re-appearing = death + respawn).

Built a physics twin + a beam search over macro-actions to check reachability. Findings:
- **Every winning route needs ≥3 dashes** (no ≤2-dash solution exists), each landed on a specific ledge.
- Under realistic serial timing jitter (±2 frames), the best routes clear only **25–40 %** of the time.

So a fixed open-loop timeline would be a *flaky* test — worse than none. Surfaced this to the owner, who
chose "push for a real clear" and pointed at the fix: **use the simulator to solve it, and print the
character position over serial for fast, exact feedback.** That is exactly what made it work.

## Serial position telemetry (no cart / no vendored-source edits)

Added `-D TELEMETRY=1` to the app: a `GameLoop` variant that each frame prints
`T <frame> <x> <y> <room.x> <room.y> <spd.x> <spd.y> <djump>`. It reads the live cart's `player`/`room`
globals via the **public `Vm::ExecuteLua`** (which runs in the cart sandbox) — so no cart edit and no change
to vendored fake-08. TX (telemetry) and RX (serial input) coexist on UART0.

This immediately paid off:
- **Spawn is exactly (8, 96), djump=1** — matches the twin.
- **The twin was already pixel-exact** on the easy stuff: standing-jump apex **y=77 (+19 px)**, run stops at
  **x=17** (blocked by the c3–4 step), dash launch spd **(3.5355, −3.5355)**, movement **2 px/frame** at spd 1
  (confirms the `for i=0,abs` inclusive move loop). All matched the hardware telemetry to the pixel.
- **The same timeline gives a byte-identical trajectory twice** → open-loop delivery is *deterministic*.

## Two twin-fidelity bugs the telemetry exposed (the crux)

Positions matched, but *timing* did not, and frame-synced dashes need timing. Raw telemetry of the first
dash: it fires at frame 378 (spd set) but the **position stays frozen at (8,96) for 3 frames**, then moves at
384. Two things the twin was missing:

1. **`obj.move(spd)` runs *before* `obj.type.update()`** in Celeste's object loop — velocity set on frame *f*
   first moves the player on *f+1*.
2. **A dash sets `freeze=2`** — `_update` returns early for the next 2 frames (fully skipped).

Without these the twin's timeline ran ~3 frames short per dash; delivering that plan, the 2nd dash's `x`
arrived while she was still airborne (djump=0) and was dropped — she reached mid-room (y=61) and fell back.
Modelled both in the twin → its per-frame plan now lines up 1:1 with the firmware frame counter.

## Frame-exact delivery → deterministic clear

- `-D INPUT_HOLD_FRAMES=1` (made the backend's hold length a compile option) → each byte is held exactly one
  frame; the driver re-sends every frame, locked to the telemetry frame counter (`plan_fc0 + 2·f`, `LEAD=2`).
  (Tested in the twin: send-every-frame at HOLD=1 reproduces the exact plan; HOLD≥2's 1-frame release lag
  pushes her into spikes.)
- Delivered the freeze-aware plan: **CLEARED (0,0) → (1,0) at t=12.05 s, frame 618.**

| | before (open-loop, jittered) | after (frame-synced) |
|---|---|---|
| clear rate | 25–40 % (twin, ±2-frame jitter) | **6/6 runs, identical frame** |
| verification | camera only | telemetry `room→(1,0)` **and** camera "200 M" |

## Verification

- **Over the wire:** `room.x/y` goes `(0,0)→(1,0)→(2,0)` — `100 M → 200 M → 300 M`. Reproduced many times,
  the same clear frames (618 and 908) each run.
- **On the glass:** bench-camera frames show the **"200 M"** and **"300 M"** room titles + the "60" FPS HUD +
  Madeline respawned in the next room — `docs/hardware/evidence/2026-07-18-celeste-200m-clear.png` and
  `…-300m-clear.png`.

## Extending to room 2 (200 M → 300 M) — one bug, wrongly diagnosed twice

Generalised the twin to any room (`set_room(rx,ry)`: auto-detect spawn from the tile-1 marker, the
`fake_wall` from tile 64; room (1,0) has neither special object — just terrain + spikes). Room 2's exit is a
top-right gap with **no ledge under it**, so the plain beam kept stalling at y=13 — its position-only dedup
pruned the `djump=1` state the final up-dash needs. A **full-state dedup** (position, `djump`, freeze,
velocity sign) in an incremental beam (`beam_solve_fast`) fixed that and found the route: a jump-heavy climb
with a single `dash-UR` near the top (113 frames).

Then two red herrings before the real fix:
- **The twin was *not* wrong.** On the first hardware run she matched the twin exactly through frame 72,
  then stuck against a wall and the dash launched from the wrong spot. It looked like a fragile
  wall-clear-then-dash, so a **jitter-robustness** re-solve was run (rank routes by clear-rate under random
  1-frame slips). Best was only ~0.47 — misleading, because…
- **…the hardware is *deterministic*** (room 1 clears identically every run), so the failure was the *same*
  every time, not random. The real cause: the multi-room driver read **one telemetry line per loop**, so at
  60 Hz the OS buffer backed up and by room 2 it was acting on a **stale frame counter** — delivering inputs
  late. Room 1 (early, ~12 s) was fine; room 2 (~17 s) had accumulated the lag.

**Fix:** drain the telemetry buffer each iteration and deliver against the **latest** frame counter. With
that, room 2 cleared first try and **reproducibly** — the same clears at frames 618 and 908, run after run.
(The robustness re-solve was unnecessary; its route works fine, so it's the one shipped.)

## A local fake-08 emulator — the exact VM, headless (`test/playtest/fake08-sim`)

To take the hand-written twin out of the trust chain, built a **native desktop build of the exact VM the
device runs** — same `components/fake08` source + the same `components/z8lua` — linked without modifying the
vendored tree (z8lua `.c` as C++; `logger.cpp`/`main.cpp` replaced by stubs, as the vendored fake-08 *test*
build does; our own headless `Host` with scripted input + a captured framebuffer). It exposes a small C API
(`sim_init/step/read/save/restore/draw/frame_rgb`) with ctypes bindings, and reads the live player via the
same `ExecuteLua`-poke trick.

**It matches the device to the pixel:** spawn `(8,96)`, standing-jump apex `y=77`, and — the real test —
**both shipped plans (`PLAN_100M`, `PLAN_200M`) clear on this exact VM** (`replay()` → the room advances).
That both validates the twin (it agreed all along) and gives a **pixel-perfect render** for a side-by-side
against the bench camera (`sim_run.mp4`; the emulator is the clean reference, the camera the real panel).

**Open follow-up — the search still runs on the twin.** Moving the *beam search* onto this VM efficiently
needs per-node savestates; `Vm::serializeLuaState` (eris) returns 0 here because the VM's `init_persist_all`
(which fills the eris `perm` table) is commented out and the main Lua state isn't reachable from the sandbox
`ExecuteLua` to set it up (a small vendored getter would do it). So the search runs on the fast Python twin —
now *proven exact by this emulator* — and the emulator is the validator + renderer. See
[`test/playtest/fake08-sim/README.md`](../../test/playtest/fake08-sim/README.md).

## Deliverables

- `test/playtest/fake08-sim/` — headless native build of the device VM (exact physics) + ctypes bindings + a run
  renderer; validates plans and produces the comparison video.
- `test/playtest/celeste/celeste_playtest.py` — self-contained: resets, starts the game, and for **each room** waits for its
  spawn and delivers that room's embedded plan frame-synced (draining telemetry so the frame clock stays
  real-time), verifying each clear over the wire; exits 0 (PASS) / 1 (FAIL). Clears **100 M → 200 M → 300 M**.
- `test/playtest/celeste/celeste_solver/` — the physics twin (`celeste_sim.py`, room-parameterised) + beam search
  (`solve.py`: `beam_solve` and the full-state `beam_solve_fast`) that produced the plans; `solve.py`
  re-prints both embedded `PLAN`s (verified byte-identical).
- Firmware: `TELEMETRY` (app), `INPUT_HOLD_FRAMES` (input backend) — both opt-in, off by default.

## Lessons

- **A twin that matches *positions* can still mismatch *timing*.** Frame-synced control is unforgiving of
  exactly the details you skip as "cosmetic" — here, object update order and the dash freeze.
- **Serial position telemetry is the highest-leverage tool on this bench.** It turned a slow, ambiguous
  camera-tuning loop into exact, over-the-wire feedback, calibrated the twin, and became the test's own PASS
  check. The owner's two hints (solve it in sim; print position over serial) were the whole unlock.
- **`Vm::ExecuteLua` reads live cart state without touching the cart or the vendored source** — the clean way
  to instrument a fake-08 cart.
- **When a deterministic system fails deterministically, don't reach for a randomness model.** Room 2's clear
  rate "0.47" sent me down a jitter-robustness path; the truth was a driver reading one line per loop and
  falling behind a 60 Hz stream. *Drain the buffer; act on the newest sample, not the oldest.* The
  reproducibility that made room 1 trustworthy was also the clue that room 2's bug wasn't random.
