# 2026-07-20 — Celeste closed-loop on the sim + the fc-scheduled input design

Goal for the session: solve Celeste with a **closed-loop policy** (`policy(state)->mask` that reads telemetry
each frame and reacts, like the Pico Racer), instead of the current open-loop trace — working room by room,
starting with room (0,0), on the sim first (zero latency). Then measure how much control latency a cleared
room survives, to decide whether device closed-loop is viable.

Everything here is **sim-side, in the working tree, uncommitted** (feature work → its own branch/PR when the
owner asks to land it). The firmware backend is designed but not yet written or flashed.

## Room (0,0) = "100 M" clears closed-loop on the sim — 90 frames, deterministic

`test/playtest/celeste/closed_loop.py` — a `policy(state)->mask` run through `../live.py`'s `drive_sim`,
mirroring `pico_racer/`. Clears room (0,0) in **90 game-frames, deterministically** (the open-loop trace
takes 93). It is a reactive **phase machine**: 3 dashes (UR, UL, UR) + 3 jumps, each firing on a **state
predicate** — grounded / rising-into-a-launch-window / djump-refreshed — never a frame index.

Two things cracked the pixel-precise air-dashes (both are the point of "closed-loop"):

- **Steer-to-launch.** The first attempt held Right through the whole post-jump rise and reached the apex
  ~2 px too far right (x≈72 vs 70); the up-left dash then launched from the wrong pixel and undershot the
  x≈35 landing ledge by ~4 px, missing it → fell into the spike pit → died. Fix: **coast into the apex**
  (stop pushing Right early so momentum decays) and dash at the apex. A launch-point position controller,
  robust to upstream drift, instead of a byte-exact arc.
- **Coyote-grace jumps.** The mid ledge ends at ~x40; walking to the x≥42 jump point walks *off* the ledge
  first, so a `grounded`-only jump condition never fired and the player fell. The open-loop trace jumps at
  x43 using coyote **grace** (already airborne, `grace>0`). Fix: jump on `grounded OR grace>0`.

The few launch thresholds live in `PARAMS` and were **tuned on the sim** (a small grid search over the
launch windows), exactly like the racer's `PARAMS` — the shipped policy stays reactive; only its thresholds
were fit. The observation is a racer-style Lua **TAIL** that pokes the reactive state (x, y, spd.x/y, djump,
on_ground, grace, wall-contact, dash_time) into scratch RAM 0x4300+, read via `sim_peek` — the shared
`sim.cpp` reader is untouched, and these are exactly the fields a device telemetry stream would carry.

Rendered filmstrips (spawn, open-loop climb, closed-loop climb) confirmed the route visually.

## The latency wall — device closed-loop is NOT viable for this room

`test/playtest/celeste/fc_latency.py :: latency_wall()` injects a fixed end-to-end lag N (apply the mask N
frames after the state it saw) and sweeps N:

| lag | result |
|---|---|
| 0 frames | **CLEAR (f90)** |
| 1 frame | FAIL — dies at the up-left dash launch |
| 2, 3 frames | FAIL |
| jitter {0,1}, {1,2}, {0,1,2}, {1,1,1,2} | **FAIL — every one** |

A **fixed** lag is compensable but razor-thin: a policy re-tuned for exactly 1 frame clears at exactly 1
frame and fails at 0 and 2 (grid-searched). But **any jitter is fatal** — a dash one frame late launches
from the wrong pixel and dies. This is the discrete/error-fatal nature of Celeste vs the racer's continuous
control (the racer merely *degraded* 62→27 under a 1-frame lag because a slightly-late steer is still
roughly right). Since the board's serial control latency is ~1 frame **and jittery** (prior racer
measurement), closed-loop-over-serial is not viable for a frame-precise room. **This confirms the plan in
[[celeste-closed-loop-next]]: closed-loop-solve on the sim + open-loop-deliver on the device.**

`fatality()`: of the 7 critical (dash/jump) commands, **5 are fatal-on-miss** (the 3 dashes + the 2 setup
jumps that feed dashes); the 2 top hops **self-recover** (phase 6 re-pulses jump when it can — the closed
loop absorbs those). So a clear needs only the 5 launch-critical commands to land.

## Where the jitter actually comes from (and the fix that keeps telemetry)

Read the firmware: the whole runtime — `vm->Step()` (which calls `input_poll`), `drawFrame()`, telemetry TX
— runs in **one task on core 0** (`app_main`, no `xTaskCreate`; IDF main-task affinity = CPU0). ESP32-S3 is
**dual-core**; **core 1 (APP_CPU) is idle**. Per-frame work is only ~6 ms (Step 2.6 + draw 3.6) of a 33 ms
frame. Telemetry is not the bottleneck — the racer already minimized it (binary + 921600). The killer is the
**±1-frame phase jitter** between the device's fixed frame clock and the host's async read/send loop: input
is sampled once per frame, and the host's reply lands at a random phase against that sample, so the same
decision is applied on frame N+1 or N+2 unpredictably.

Two dead ends worth recording:

- **"Just offload serial to core 1"** does *not* fix the jitter. `input_poll` already drains all available
  bytes at poll time; which core reads doesn't change *which frame* applies the byte.
- **Predicting via VM savestate** does not work here — `sim_save`/`sim_restore` (and the device's
  `serializeLuaState`) are the **parked, disabled eris path** (`init_persist_all` commented out; returns 0).
  The host can't snapshot the VM to roll forward. The right predictor is a **lockstep twin** (below).

**The design (does not abandon telemetry):** the host tags each command with a **target frame** (`fc`, which
it already reads from telemetry, + a lead k) and the device applies it when its frame clock *reaches* that
`fc` — not when the byte arrives. Host jitter then only has to **beat a deadline**, not hit an instant; the
apply is deterministic. A **core-1 UART task** drains the wire continuously into a lock-free SPSC latch
(hardening — max deadline margin, game task never touches the UART), and the apply-by-`fc` logic runs in
`input_poll` on core 0. Wire format + scheduler live in `test/playtest/fc_sched.py`; the firmware backend is
`components/input/input_scheduled.c` (specced, not yet written). The on-device **miss-rate**
(`g_miss_count/g_applied`) streamed in telemetry *is* the measurement this turns on.

## Sim validation of the fc-scheduled design (before any flash)

`test/playtest/fc_sched.py` (protocol + `DeviceScheduler`) + `celeste/fc_latency.py`:

- **Protocol/scheduler** unit-tested (`test/playtest/test_fc_sched.py`, 6/6): 8-byte framed command
  `0xA6 | fc:u32 | mask | hold | xor-csum`; parse resyncs past junk/bad frames and across split reads;
  apply-at-`fc`, hold windows, and miss counting all correct.
- **Lockstep-twin predictor — EXACT.** The host twin (same deterministic policy from spawn, fed the
  committed command stream) reproduces the device's `(x,y,dj,grnd)` **k frames ahead exactly** at every
  checkpoint. So the twin computes the action *for the target frame it schedules* → zero effective lag with
  k frames of delivery slack, and **no policy re-tuning is needed**. Telemetry is only used to detect a miss.
- **Clean pipeline** through the real `encode_cmd → DeviceScheduler → apply` clears at f90 (0 misses).
- **Jitter sweep** (host round-trip R = baseline + occasional spike, three realism levels), clear-rate over
  150 trials/cell through the real protocol:

  | host jitter | k=1 (33 ms) | k=2 (67 ms) | k=3 (100 ms) |
  |---|---|---|---|
  | tight (binary+921600+low_latency) | 100% | **100%** | 100% |
  | realistic (binary, default USB timer) | ~92% | **100%** | 100% |
  | loose (ASCII, 115200, untuned) | ~72% | ~89% | **100%** |

  **k=2 lead + the serial tuning the racer already ships → 100%** across realistic host jitter (worst R seen
  ~58 ms vs the 67 ms deadline). The Monte-Carlo model and the real-protocol run agree.

## Landed this session (working tree, uncommitted)

Sim side (host + protocol):
- `test/playtest/fc_sched.py` — shared, cart-agnostic: the wire protocol + `DeviceScheduler` (the Python
  twin of the firmware backend).
- `test/playtest/test_fc_sched.py` — protocol/scheduler unit tests (pure Python, 6/6).
- `test/playtest/celeste/closed_loop.py` — the reactive room-(0,0) policy (clears the sim, 90 frames).
- `test/playtest/celeste/fc_latency.py` — latency wall + fatality + lockstep-twin predictor + fc jitter sweep.

Firmware side (written + host-logic-validated; not yet built for the ESP32 or flashed):
- `components/input/input_scheduled.c` — the backend: lock-free SPSC ring, `fc_rx` task pinned to APP_CPU,
  apply-by-`fc` in `input_poll`, deadline miss counter (`input_sched_stats`), byte-framed parser w/ resync.
- `components/input/input.h` — `input_set_frame(uint32_t)` added to the seam; no-op'd in the other four
  backends (stub/serial/touch/i2c) so the seam stays complete and `main.cpp` can call it unconditionally.
- `firmware/pico-e32-fake08/main/main.cpp` (+ its `CMakeLists.txt`) — `#include "input.h"`, `input` added to
  `REQUIRES`, and the hook `input_set_frame(GetFrameCount()+2)` before `Step` in the telemetry loop (kept out
  of the `step_us` window). Boot order checked: the CFG handshake reads UART0 before the first `Step`, so
  `fc_rx` (spawned lazily on that first `Step`) never contends for CFG's bytes.
- `components/input/host_test/` — host gcc compile + logic test of `input_scheduled.c` against stub ESP-IDF
  headers (`run.sh`). Compiles clean (`-Wall -Wextra`) and its ring/parser/apply-by-`fc`/miss logic **matches
  the Python twin** on the same vectors (apply-at-fc, expire, miss, hold window, OR, resync). This pins both
  implementations to one protocol without hardware.

- `test/playtest/celeste/fc_device.py` — the host driver: delivers the twin's masks as `fc`-tagged commands
  (lead k) to the board; anchors at the room-(0,0) spawn and verifies the clear over telemetry.

## HARDWARE VALIDATED — room 1 clears closed-loop on the board via fc-scheduled input

**Deterministic clear on the physical Makerfabs ESP32-S3, room (0,0) -> (1,0) at fc=358, 3/3 runs identical.**
Bench-camera video: the panel visibly plays room (0,0) "100 M" and clears (colours cast by `awb=0` — judge
shape; the room + "30 FPS" HUD are unambiguous). Verified over the wire (telemetry rx/ry 0,0 -> 1,0), like the
open-loop play-test. This closes the arc: sim closed-loop -> latency wall -> fc-scheduled design -> firmware
-> real hardware.

**Adversarial review BEFORE flashing caught real bugs (a multi-agent workflow over concurrency / ESP-IDF-API
/ protocol / integration; 8 confirmed of 10).** Fixed before any hardware:
- **CRITICAL** — `fc_rx` used `uart_read_bytes(buf, 64, portMAX_DELAY)`, which (verified against vendored
  ESP-IDF 5.4.2 `uart.c`) blocks until ALL 64 bytes arrive, not the first. The host trickles single 8-byte
  commands, so nothing would ever parse in time — the feature would silently no-op on hardware. Fixed: block
  on the first byte, then drain the rest non-blocking (matches `input_serial.c`).
- **Frame-exactness off-by-one** — `input_set_frame(GetFrameCount()+2)` was wrong: `vm->Step()` advances the
  counter by **1** (a 30 fps cart = 2 Steps/game-frame), and telemetry emits it right after Step, so the fc a
  Step emits is `before+1`. Fixed to `+1u`.
- **Miss boundary** — the drain miss-check was `c.fc <= G`, which wrongly dropped a command arriving exactly
  on its target frame. Fixed to `c.fc < G` (== is on time). Twin (`fc_sched.py`) + the pipeline model +
  boundary tests updated to match.
- Hardening: UART-error backoff (no tight loop), ring-drop counter folded into the stats miss-rate.

**Two firmware-config gotchas (cost real time — recorded so the next session doesn't repeat them):**
1. **`-D` DEFS persist in the CMakeCache.** A first build with `TELEMETRY_HOST_CFG/BINARY/BAUD` left those set
   even after dropping them from DEFS (the CMakeLists is `if(DEFINED X)`), so the board kept streaming BINARY
   telemetry at 921600 while the host read ASCII at 115200 → looked dead. Fix: clear them from the cache (or
   `make fullclean`) when switching telemetry mode.
2. **A new `-D FOO` needs a `target_compile_definitions` rule in `main/CMakeLists.txt`** — `-D CELESTE_START=1`
   was a no-op until the rule was added (like `CELESTE`'s), so the autostart silently didn't compile in.
3. **A `fullclean` boot-looped the board — the repo's `sdkconfig.defaults` didn't reproduce the validated
   config.** All prior builds were incremental on an old build dir whose `sdkconfig` had compiler assertions
   OFF; `make fullclean` regenerated a fresh `sdkconfig` with assertions ON (IDF default). On this esp-idf/newlib
   the FIRST `printh` at boot (`fwrite` → newlib recursive stdio lock `__retarget_lock_acquire_recursive` →
   `xSemaphoreTakeRecursive`) trips FreeRTOS `configASSERT` (= plain `assert()` here — no separate FreeRTOS
   knob), so every ASCII-telemetry build boot-loops. Backtrace decode (`xtensa-esp32s3-elf-addr2line` on the
   ELF) pinned it: `app_main → ExecuteLua → fwrite → lock_acquire_generic → assert`. The strict lock check is
   benign here (the stdio write works; the board cleared Celeste + measured fps under assertions-off). **Fix:
   pinned `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE=y` in `firmware/pico-e32-fake08/sdkconfig.defaults`
   (a perf/dev board with `COMPILER_OPTIMIZATION_PERF=y` → release-style, assertions off) so a clean build now
   reproduces the validated config.** Follow-up: the underlying newlib stdio-lock check firing at all is worth a
   look, but it's orthogonal to the closed-loop work.

**Firmware build/flash (validated):**
```
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<CP2104> \
  DEFS='-D CELESTE=1 -D CELESTE_START=1 -D FORCE_FLASH_CART=1 -D INPUT_BACKEND=scheduled -D TELEMETRY=1 \
        -D SHOW_FPS=1 -D CENTER_GAME=1'
python3 test/playtest/celeste/fc_device.py <CP2104>     # -> PASS: room (0,0) cleared via fc-scheduled input
```
`CELESTE_START` = `begin_game()+load_room(0,0)` at boot (skip the title — the scheduled backend takes only
fc-commands, not the raw start key). Plain ASCII telemetry (no binary/baud — telemetry jitter is irrelevant to
fc-scheduling; it's not in the control loop). The host driver needs `settle>0` (lead for the first command;
the player is static at spawn, so any lead works — `settle=8`).

## Twin-in-the-loop predictive closed loop — the board genuinely in the loop (general, cart-agnostic)

`fc_device.py`'s first pass shipped a FIXED twin-solved mask sequence (closed-loop-solved, frame-scheduled-
delivered) — the board wasn't reacting. `live.drive_device_predictive` (cart-agnostic) closes the loop on the
board: a deterministic **twin** runs the policy leading the board by k frames, streaming each frame's mask as
an fc-command; every telemetry frame the driver **compares the board's real state to the twin's prediction**
and, on divergence, **rebases the twin to the board and re-plans**. A cart supplies `policy`, a `twin`
(reset/read/step/seed), and telemetry `parse`/`at_start`/`is_done`/`diverged` (+ `twin_done` to bound the
twin — the fake08 VM is not teardown-safe across a clear/death, so grow() must stop at the twin's own done).

**Verified on the board: room (0,0) clears via the predictive driver, 3/3 deterministic (fc=358), 0
divergences.** The board is monitored against the twin every frame; this clean deterministic room needs no
correction, so the reactive path and the fixed delivery agree — but the mechanism is genuinely reactive and
generic. Honest limits: (1) Celeste is deterministic → the rebase/recovery layer isn't exercised here; it
shines on carts with rnd/fps drift (racer-class). (2) The rebase resets the twin VM but not a STATEFUL
policy's hidden phase — clean for a stateless `policy(state)` (the racer's `choose_mask`), but Celeste's phase
machine would need its phase reconstructed (replay from spawn) on a rebase. Both are the next step toward
"closed-loop play-test on ANY cart": wire the racer through `drive_device_predictive` to exercise rebase
recovery on a continuous cart.

Two bugs found + fixed getting it working on the board: the twin path never called `VM.init` (segfault on an
uninitialized VM), and the delivery loop skipped commands until `gf>=0` (so the first commands went out with
no lead → missed → the player never moved). Both fixed; `grow()` is now bounded by `twin_done`.

**Rebase/recovery, tested on the board (inject a miss with `drop_frames`, `resync` on/off) — the mechanism
fires but frame-precise recovery FAILS, and that sharpens the answer:** the driver now rebuilds a stateful
policy on a rebase (replay it over frames 0..gf-1 to reconstruct the phase). Dropping 2–8 mid-run commands:
with `resync=True` the rebase fires every time (3 divergences → 3 rebases) but the room never clears; with
`resync=False` the board just stays diverged (~100 mismatched frames). So on a frame-precise cart, recovery
from a miss does NOT work — a small position error compounds through the pixel-exact dashes, and the seed
(telemetry x/y/spd, no sub-pixel rem) can't re-converge. This is the same error-fatal wall from the other
side. **Conclusion → "closed-loop on ANY cart" is the RIGHT STRATEGY PER GENRE, not one universal recovery:**
FRAME-PRECISE carts must AVOID misses (fc-scheduled + lead k → ~0 misses; recovery is futile) — proven on the
board; CONTINUOUS carts (racer) can lean on live-reactive / rebase recovery (a late steer is recoverable).
The twin-in-the-loop driver serves both; only the reliance differs.

## Racer through the SAME driver on hardware — recovery WORKS on a continuous cart (the "any cart" close)

Flashed the racer (`-D RACER=1 -D RND_SEED=39 -D INPUT_BACKEND=scheduled -D TELEMETRY=1 -D TELEMETRY_HOST_CFG=1`),
wrote a racer adapter (`pico_racer/racer_fc_device.py`: stateless `choose_mask` policy, twin = racer sim VM
w/ srand pin, seed()=set px/pt, ASCII CFG tail tpos/px/speed/pt/clock/gamemode), and ran it through the SAME
`live.drive_device_predictive`. **The racer drives closed-loop on the board (tpos 12–13 baseline).** Then the
recovery test — drop 45 steering commands during active racing:

| | final px (on-road = ±20) | divergences | rebases |
|---|---|---|---|
| **WITHOUT recovery** | **214** — runaway off the track | 2551 (uncorrected) | 0 |
| **WITH recovery** | **14** — on the road | 61 | 61 |

**So recovery WORKS on a continuous cart** — a big miss sends the car off the track uncontrolled, and the
rebase (seed px → the stateless policy re-steers) keeps it on the road. This is the exact opposite of Celeste
(recovery fired but the frame-precise run never re-converged). The complete "any cart" picture:

  * FRAME-PRECISE (Celeste): must AVOID misses (fc-scheduled + lead k → ~0 misses); recovery from a miss is
    futile (error-fatal). Proven on the board (clears, deterministic).
  * CONTINUOUS (racer): forgiving; a small drift self-tolerates, and a BIG miss is recovered by the rebase.
    Proven on the board (predictive: on-road under an injected miss). Also already closed-loop via the simpler
    live-reactive `drive_device` (M9). Robustness comes from the RIGHT STRATEGY PER GENRE, not one universal
    recovery — and the one `drive_device_predictive` serves both genres.

Bugs fixed for the racer: `has_state` was Celeste-specific (`'x'`) and gated racer delivery to zero → made it
a cart-supplied predicate (default: any parsed frame); and the start needs `settle>0` lead (same as Celeste)
or the start `X` misses and the race never begins. Honest caveat: the rebase is replay-from-root O(gf), so it
is EXPENSIVE for long continuous runs (rebasing slows forward progress — tpos is lower *with* recovery even as
control is better); a working O(1) savestate (parked eris) would fix that, and for continuous carts the
live-reactive `drive_device` (M9) is the lighter pragmatic path anyway.

## Board recovery + the title-skip that replaces CELESTE_START + the confirmed on-hardware Celeste fps

A `make fullclean` (forced after the CMakeCache `-D` corruption) regenerated a fresh `sdkconfig` that did **not**
reproduce the validated build — two settings were missing from the tracked defaults, and the board boot-looped,
then froze. Both are now pinned in `firmware/pico-e32-fake08/sdkconfig.defaults` (so a clean build reproduces the
demo config), diagnosed in order:

1. **Boot-loop** — assertions defaulted ON; the first `printh` at boot trips a benign newlib recursive-stdio-lock
   `configASSERT`. Fix: `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE=y` (perf/dev board). See the config
   gotchas above.
2. **Freeze (the real root cause)** — with the assert masked, telemetry streamed but the cart's Lua coroutine died
   right after boot (`fc` frozen at 1, no player). The fresh config had defaulted the ESP32-S3 **secondary console
   to USB-Serial-JTAG**; that second stdout path's recursive lock is what the assert had been catching, and with it
   masked the corruption killed the cart's first `printh`. This board talks over its CP2104 **UART only**. Fix:
   `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`. Board boots clean, `fc` advances, telemetry streams.

**Title-skip without CELESTE_START.** Under the fresh config the boot-time `CELESTE_START` (`begin_game()` +
`load_room(0,0)` via `ExecuteLua`) logs but does not stick — a boot-time room-load does not survive the title's
own deferred init. Rather than chase that, the driver now **starts the game the way a player does**, over the same
fc-scheduled backend it plays with: a short `warmup` pulses jump commands to press "start", then stops and lets the
spawn animation land at (8,96). This is a new, backward-compatible `warmup=` hook on `live.drive_device_predictive`
(the racer needs none — its twin models the countdown, so the policy's own early masks are the start input) and an
inline title-skip in `fc_device.drive_device`. `-D CELESTE_START` is no longer required in the build.

**Confirmed on the recovered board** (`fc_device.py --predictive`, twin-in-the-loop):

```
predictive: delivered 90 fc-commands, 0 divergence(s), 0 rebase(s)  ->  CLEAR at fc=464
fps over 228 game-frames (target 30): achieved min/avg/max = 9.8/29.8/30.0
                                       headroom min/avg/max = 9.8/66.0/118.3
```

Clean run — the lockstep twin matched the board frame-for-frame (no divergence). Filmed fresh on the recovered
board via `tools/record_video.sh -- python3 test/playtest/celeste/fc_device.py <board> --predictive` (7 s,
de-distorted + upright; throwaway `/tmp` per bench convention — the pre-recovery footage is now superseded).
Non-predictive `drive_device` (pre-solved masks over the same backend) also clears at fc=464, 90/90. The
**9.8/29.8/30.0** matches M8's
open-loop Celeste fps (~9.2/29.9/30.0): fps is the cart's Step+draw compute, independent of the input shape.
Celeste holds the 30 fps cap on average (~half the frame budget spent; 66 fps avg headroom); the min-frame dip
is the room-transition compute spike. This is the "re-capture Celeste's fc-scheduled fps" line item, closed.

## Room (1,0) = "200 M" closed-loop — sim + device chain (the second room)

Second room, same reactive-phase style as the room-(0,0) Climber (every move fires on a STATE predicate, never
a frame index). Room map (dumped via `fget(mget(...),1)`): a LEFT channel (cols 4-6) between the col-3 wall and
the col-7 pillar; a gap (cols 8-9) past the pillar; a RIGHT shaft (cols 8-12) to the exit (top, cols 12-14).
`Climber200` (in `celeste/closed_loop.py`) phases: APPROACH the channel -> wall-jump CLIMB it -> **LAUNCH off the
left wall** (the non-obvious bit: the rightward momentum to cross comes from a left-wall jump, not the pillar) ->
COAST right + cross-DASH over the pillar (fires at x71 y46, matching the open-loop reference's x71 y43) -> RCLIMB
the right wall to the exit. Clears the sim in 145 frames, deterministic. `PARAMS200` tuned on the sim.

**The device plays a CHAIN, not an isolated room.** The board reaches room N only by clearing N-1, so a `Chain`
policy plays the whole run and switches to the right per-room policy on the live room; `drive_sim_chain` /
`fc_device.predictive(rooms=((0,0),(1,0)))` drive it. One non-obvious bug the chain exposed: clearing room (0,0)
drops the player into room (1,0) **at the top (x110,y-2)**, freezes through the transition, then respawns it at
the bottom (8,112) — so `Climber200` cascaded through all its phases on the bogus entry state and was stuck in
its last phase by the time the player actually spawned. Fix: a `spawned` gate holds neutral until the player is
grounded on the spawn floor (harmless in isolation, where it already is). With it, the chain clears both rooms.

**On the recovered board** (`fc_device.py --chain`, same firmware as the room-(0,0) test — no reflash):

```
predictive: delivered 260 fc-commands, 0 divergence(s), 0 rebase(s)  ->  CLEAR at fc=806  (final room 2,0)
fps over 399 game-frames (target 30): achieved 9.6/29.8/30.0  |  headroom 9.6/62.3/117.8
```

Both 100 M and 200 M cleared closed-loop, twin-in-the-loop, perfect lockstep (0 divergences over the whole
two-room run). Filmed fresh (`tools/record_video.sh -- ... --chain`, 18 s, throwaway `/tmp`). fps unchanged from
the single-room number — it's the cart's Step+draw compute, independent of input shape or room count.

## Room (2,0) = "300 M" — solved on sim (VM beam search) + device via OPEN-LOOP (not the twin)

Third room, a hazard+spring PUZZLE: two up-facing spike fields, springs required, and the exit sitting directly
above the upper spikes so the only clean approach is the right shaft (col 11). No open-loop reference existed and
the `celeste_solver` physics twin can't help — it models terrain + spikes but **not springs**, so all its beam
"wins" fail to VM-verify. Route found instead by a **beam search on the REAL fake08 VM** (`scratchpad/beam300.py`,
full physics incl. springs; VM.spawn is ~15 ms and savestates are broken, so it replays-from-root with a small
beam ~70; pure min-y stalls below the spikes, so the fitness steers toward the exit column x~80 when high).
Reverse-engineered into `Climber300` (`closed_loop.py`, `ROOMS[(2,0)]`): dash R into spring1 → ride the bounce →
DASH UR at the apex to the right wall → two wall-jumps up → DASH UP into the exit gap → wall-jump through. Clears
the sim deterministically (77 f isolation; chain 100→200→300 = 362 f → room (3,0)).

**Device: the twin-in-the-loop FAILS 300M, but OPEN-LOOP replay CLEARS it.** The predictive driver's REBASE is
what breaks a frame-precise room: 300M's dashes (speed + a 2-frame freeze) transiently exceed the 6px divergence
threshold, so it re-plans and disrupts the delivery — and a missed pixel-dash is unrecoverable regardless.
`drive_device_chain` (`--openloop`) just streams the pre-solved chain masks fc-tagged with no feedback; on the
deterministic board that clears the whole chain, **CLEAR at fc=1016, 3/3 identical**:

```
open-loop 100 M -> 200 M -> 300 M: delivered 362/362 fc-commands  ->  CLEAR at fc=1016
```

So for a deterministic fc-scheduled board, **open-loop replay > twin-in-the-loop for frame-precise multi-room
chains** — it extends the earlier "recovery is futile for frame-precise" finding to "recovery is *counter-
productive* there." Diagnosis that got us here (adversarial `Workflow` over firmware + fake08 VM + input backend
+ driver, cross-checked against board telemetry captures): `fc = _picoFrameCount`, exactly 2/game-frame **including
dash freezes** (the 30 fps yields sit outside `_update`, so a freeze early-return doesn't change the cadence — an
early freeze-drift hypothesis was wrong); board and twin hit every room spawn at the *same* game-frame; a ~1-frame
fc offset from the room-transition flash is invisible to `diverged()` and fatal only to 300M's apex-dash under the
predictive path. Fixes that tried to correct it under the predictive path (per-room / last-room-only plan_fc0
re-anchor, larger lead) all disturbed the working rooms and were reverted (removed from the driver). Combined
sim|device videos: `celeste/videos/{100m,200m,300m}.mp4` (all open-loop device now); tool
`celeste/render_compare.py`.

## Room (3,0) = "400 M" — solved on sim + device (OPEN-LOOP); a RAW-MASK room (2026-07-21)

The fall-floor room. Not reactive-`Climber`-shaped — the route is a pixel-precise wall-jump chain, so it ships as
a **raw mask list** replayed by a new `ReplayClimber` (`closed_loop.py`, same `spawned` gate; registered
`ROOMS[(3,0)]` via `make_replay(ROUTE_400, spawn)`). Two VM-verified routes in `celeste/routes/`; the shipped one
(`room400_altroute_viaD.json`, 189 f) matches the user's hand-drawn route — it lands *on* the top-right block D.

Three non-obvious physics facts, found the hard way (user-guided, and my first two reads of the room were wrong):
- **Fall-floors crumble the instant you leave** → the staircase climb (B→E→F→C) is continuous **jumps only**; the
  dash is saved for the exit.
- **D (a fall-floor) is solid from BELOW** → a straight-up dash bonks its underside (~5 px short); you mount it
  with a **wall-jump off its side wall**. `is_solid` doesn't see fall-floor *objects*, so the telemetry `wall`
  flag reads 0 near it — the wall-jump has to be found empirically (it fires: spy→−2).
- **The exit only opens through cols 5-7**, gated by a solid ceiling over cols 8-15 → the launch must come off the
  "1" block top; anything to the right dead-ends at y5.

Route search (scratch, throwaway): a jump-only K-landing beam over the fall-floor staircase, then a
**placement-seeded** search of the top — teleport the player to the C-landing on a fresh spawn (D + block are
untouched by the climb, so still solid) to skip the 95-frame climb replay per try (`VM.step_mask`≈1 ms, savestates
broken → replay-from-root only).

**Device: CLEAR.** `--openloop --to400` (100→200→300→400 M) clears on the board at **fc=1454, 723 game-frames, fps
7.3/28.4/30.0** — the frame-precise wall-jump route survives blind fc-scheduled replay, exactly like 300 M. Synced
sim\|device video `celeste/videos/400m.mp4`.

## Open / next

- **Room (4,0) = "500 M"** — key+chest navigation room with two up-spike fields; the exit is another
  400 M-D-style wall-jump onto an isolated block (climb + wall-jump + exit-dash all confirmed on the VM), PAUSED
  at the climb→wall-jump join (~7 px short of the launch spot). Scratch beam/connect tools exist.
- **Stateful-policy rebase on frame-precise** — even reconstructing the phase, Celeste can't re-converge a
  pixel-exact dash from a sub-pixel-lossy seed; recovery there is fundamentally futile (documented), so the
  answer for frame-precise is fc-scheduled (avoid misses), not recovery.
- **O(1) rebase** — the replay-from-root rebase is costly on long runs; needs the parked eris savestate.
- **Miss-rate readout on-device** — `input_sched_stats()` counts fed/miss/applied, but the plain ASCII
  telemetry tail doesn't stream them yet. Add them to the tail (or a periodic `ESP_LOGI`) to read the real
  deadline miss-rate per lead k and vs host `low_latency` — the last quantitative number (the clear already
  proves misses are ~0 at k=2 on this path).
- The savestate/eris path is still disabled here — prediction stays on the lockstep twin.
- Caveats: the R distributions are plausible estimates, not measured on this board; the lockstep twin is
  exact only absent divergence (a miss desyncs it until the next telemetry frame — but a launch-critical miss
  already kills the run); room (0,0) only so far.
  (rooms 100→400 M now all clear on sim + board; 500 M is the paused frontier, above.)
