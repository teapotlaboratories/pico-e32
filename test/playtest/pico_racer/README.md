# Pico Racer — play-test gym solve (M9)

A score-maximizing solve for **`assets/Pico Racer.p8.png`** (a pseudo-3D racer by kometbomb), driven and
verified on the exact device VM (`test/playtest/fake08-sim`). The deliverable is a deterministic,
replay-able **Trace** of per-game-frame button masks.

## The cart & the objective
Pico Racer is a lap-less arcade racer: a `starttimer` 3-second countdown, then a `clock` that ticks down
(~46 s). There is no "win" — the goal is **distance**: reach the highest track segment `tpos` (1..141)
before `clock <= 0`. The cart has no player/room objects, so the shared Celeste `read()` is useless; racer
globals are read straight out of the VM via `sim_exec` + `sim_peek` into scratch RAM `0x4300..` (which the
cart never touches).

Verified mechanics (from the decompiled cart):
- 30 fps cart -> `steps_per_frame = 2`. Accelerate = UP/X, brake = DOWN/O, steer = LEFT/RIGHT.
- `px` = lateral offset from track center. `abs(px) >= 20` bleeds speed; `abs(px) >= 32` at low speed
  triggers a crash/reset. Curves push `px` (`xpush = curve*32*speed`), so you must counter-steer.
- Steering has **momentum**: `pt` (steering velocity) changes by +-0.6/frame and saturates at +-2, and
  `px -= pt`. So the max curve you can hold is `|xpush| <~ 2`; tighter bends require slowing down.
- **Goals** (gate object type 16) add **32 s (960 frames)** to `clock` and require no alignment — just
  reaching them. Banking goals (at tpos 17, 36, 49, 62, ...) is what lets a run go far.
- **Cars** (type 3) quarter your speed on contact (`speed *= 3/4`) and, near `carspeed`, linger and hit
  repeatedly. **Rivers** (type 6) quarter your speed unless you are airborne; **jump ramps** (type 12)
  launch you (`jump=3`) so you clear the river un-slowed.

## Approach
A closed-loop controller runs ON the sim, reading `px`, `speed`, `pt`, the current/upcoming curve, and the
object queue each frame, and emits a held-button mask. Three rules cooperate (see `solve.py:choose_mask`):
1. **Steer** to a target lane with `pt* = xpush + K*(px - target)` (cancel the curve, pull toward target).
2. **Ease** speed with a per-curve cap `hold/(|curve|*32)` so the line stays holdable through tight bends
   (only *verytight* curves actually slow us).
3. **Avoid**: dodge an approaching car's collision band, and steer into the nearest jump-ramp lane before a
   river so we launch over it.

Because the sim is deterministic, the recorded per-frame masks are a faithful **open-loop** replay.

## Determinism (the important part)
fake-08 seeds `rnd()` from **wall-clock time** on every cart load (`vm.cpp` `api_srand(... now ...)`), and
the cart spawns cars/obstacles with `rnd()` — so a raw run is non-reproducible. Two fixes make the Trace
replay exactly:
- **`srand(SEED)`** right after `VM.init` pins all car/obstacle spawns. The seed is stored in the Trace meta
  and re-applied by the verifier.
- **One `VM.init` per process** + a prepended **neutral frame** (mask 0) so the start-button edge is clean
  regardless of prior process state. (Re-init in the same process leaks prior VM state in fake-08; only the
  first init is canonical — which is why `verify_solution.py` runs as its own fresh process.)

Reads via `sim_exec`/`sim_peek` only poke scratch RAM `0x4300..` and are state-neutral, so the closed-loop
run and its exec-free replay agree to the frame (checked).

## Result
- **Final `tpos = 63 / 141`** (~45% of the track — through Egypt legs 1-3 and into North Pole leg 1),
  banking 3 goals; **`clock = 0`** at the end; **0 off-road frames, 0 crashes**.
- This is ~4x the `tpos ~15` bang-bang baseline. Seed `39` (chosen by a sweep; the seed is part of the
  fixed, documented setup).

## Reproduce
```sh
# from the repo root:
python3 test/playtest/pico_racer/solve.py               # -> solution.trace.json (tpos 63)
python3 test/playtest/pico_racer/verify_solution.py     # fresh-process open-loop replay + filmstrip
```
`verify_solution.py` asserts the replayed `tpos` equals the banked value and writes a sampled filmstrip
PNG to `/tmp/pico-racer-verify/verify_run.png` (watch the car track the road and the TIME counter jump each
time a goal is banked). `solve.py --seed N` tries a different rnd seed.

## On the device (`racer_playtest.py`)

The racer runs on the real board too. Firmware (`-D RACER=1` embeds `assets/Pico Racer.p8.png` as the flash
cart — the gitignored `assets/pico_racer_p8.h`, mirroring the Celeste build; the `Cart` ctor decodes the
`.p8.png`):
```sh
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<board> \
  DEFS='-D FORCE_FLASH_CART=1 -D RACER=1 -D TELEMETRY=1 -D TELEMETRY_HOST_CFG=1 \
        -D TELEMETRY_BAUD=921600 -D TELEMETRY_BINARY=1 -D TELEMETRY_BINARY_BYTES=40 -D RND_SEED=39 \
        -D INPUT_BACKEND=serial -D INPUT_HOLD_FRAMES=2 -D SHOW_FPS=1 -D CENTER_GAME=1'
```
Telemetry is **cart-agnostic**: the firmware carries no per-cart tail — `racer_playtest.py` sends the state
expression at startup over a `CFG?` handshake (`-D TELEMETRY_HOST_CFG=1`, dev/HITL-only, compiles out of
production). The racer streams a rich observation (steering + the two nearest cars + the nearest ramp/river +
the worst upcoming curve) so the controller can DODGE. Because the dodge is control-latency-sensitive, two
levers are on by default: **`TELEMETRY_BAUD=921600`** (runtime-switch after the handshake — less serialization
jitter) and **`TELEMETRY_BINARY=1`** (ship the observation as raw sync-framed int16s instead of an ASCII line).
`RND_SEED` pins PICO-8's `rnd()` (via `srand`, replayed after a matching 60-step boot); `INPUT_HOLD_FRAMES=2`
holds each per-game-frame byte across both Steps of the 30 fps cart (=1 keeps a *held* key alive only one of the
two Steps → the car won't accelerate).

**Two device modes** — the important lesson of M9:
- **Closed-loop (default, robust):** `python3 test/playtest/pico_racer/racer_playtest.py <board>` runs the
  `policy(state) -> mask` LIVE over serial — read the observation each frame, steer/brake/dodge in response.
  Feedback self-corrects, so rnd/fps/timing differences don't matter; the car dodges traffic, seeks jump-ramps
  before rivers, and eases through tight bends, **reaching tpos 43–44 at clock-out** (the sim ceiling is 62; the
  residual gap is the device's control-loop latency + jitter, not the policy — which is why the baud + binary
  telemetry are the default). The **same policy** runs on the sim with `--sim` (via the shared `../live.py`
  runner) — dual-target, so it's developed + verified with no board (`drive_sim` and `drive_device` share the
  one `policy`; the sim reaches tpos 62–63).
- **Open-loop trace replay (`--replay`, fragile):** frame-syncs the solved Trace. Deterministic on the sim,
  but on hardware any small divergence (a 1-step boot offset, an fps dip, one mismatched `rnd()` car) makes
  the recorded steering wrong for the *actual* `px`, and with no feedback the error **compounds** until the
  car flies off the road. Open-loop replay suits short, discrete, self-checking tasks (Celeste rooms) — not a
  49-second continuous-control run. This is why the device driver is closed-loop.

## Files
- `solve.py` — the controller; drives the sim VM and emits the Trace (open-loop solution).
- `solution.trace.json` — one segment `"run"` of 4420 per-frame masks; `meta` carries the seed + final tpos.
- `verify_solution.py` — authoritative fresh-process open-loop sim replay + filmstrip.
- `racer_playtest.py` — the `policy(state)->mask` + a sim/device state reader; drives the SAME policy on the
  sim (`--sim`) or the board (default) via `../live.py`. `--replay` keeps the (fragile) open-loop trace path.
