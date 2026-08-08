# fake-08 input — a compile-time-switchable input driver (with a serial backend for HITL)

The port's input seam. fake-08's `Host::scanInput()` returns a PICO-8 button state
(`InputState_t{KDown, KHeld}`, bits `P8_KEY_LEFT..PAUSE` = 0..6). This doc is the plan for filling
that seam with a **compile-time-selectable backend**, so the same `scanInput()` can be driven by the
board's touch panel, an I²C button expander, or a **serial link** — the last of which is what makes
input **hardware-in-the-loop testable** (no human finger, no parts).

Indexed from [`docs/pico-e32-todo.md`](../pico-e32-todo.md); the parts-blocked status it supersedes is in
[`pico-e32-fake08-port.md`](pico-e32-fake08-port.md) (Input row). Plan-first per
[`.ai/AGENTS.md`](../../.ai/AGENTS.md).

## Why a switchable driver — and why serial first

`scanInput()` is the one seam every input source flows through. Touch and buttons can't be actuated by
an automated bench (the rig only *observes* the panel), so an input path that the agent can **drive over
the wire** is the only way to verify the whole `_update`/`btn`/`btnp` path end-to-end. Build the seam +
the **serial** backend first (fully verifiable), and touch / I²C drop in behind the same interface.

## The seam

One tiny C interface (`components/input/input.h`); exactly **one backend** is compiled per build:

```c
enum { INPUT_LEFT=1<<0, INPUT_RIGHT=1<<1, INPUT_UP=1<<2, INPUT_DOWN=1<<3,
       INPUT_O=1<<4, INPUT_X=1<<5, INPUT_PAUSE=1<<6 };   /* == fake-08 P8_KEY_* order */
esp_err_t   input_init(void);          /* once, at first scanInput */
uint8_t     input_poll(void);          /* the currently-HELD mask, once per frame */
const char *input_backend_name(void);  /* for the boot log */
```

`ESP32Host::scanInput()` calls `input_poll()` for the held mask and computes the `KDown` edge itself
(`held & ~prevHeld`) — that logic is shared across all backends, so a backend only reports "held".

## Backends

| backend | `INPUT_BACKEND=` | wiring | status |
|---|---|---|---|
| **stub** | `stub` *(default)* | none | current behaviour — `KDown=KHeld=0`, pause menu never opens. Keeps the default build unchanged. |
| **serial** | `serial` | UART0 (the CP2104 console) | **IN-1** — bytes over the console UART → buttons. The HITL path. |
| **touch** | `touch` | FT6236, I²C 0x38 (SDA38/SCL39) | **IN-2** — on-screen d-pad + O/X zones. Real on-board input, no parts. Skeleton today. |
| **i2c** | `i2c` | I²C GPIO expander (addr ≠ 0x38) | **IN-3** — physical buttons. Parts-blocked. Skeleton today. |

## Compile-time switch

Same lever as `CELESTE`/`MEASURE_FPS`. `components/input/CMakeLists.txt` compiles `input_${INPUT_BACKEND}.c`:

```
make build APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 DEFS='-D INPUT_BACKEND=serial'
```

Default is `stub`, so a plain `make build` behaves exactly as before (no input, no UART driver installed).
(`-D` lands in `CMakeCache.txt` and persists — pass it each time, or `make fullclean` to change it.)

## Serial protocol (the `serial` backend)

One byte per key, over the console UART (115200, `/dev/ttyUSB1` = the board's CP2104). A key is **held
for ~`HOLD_FRAMES` frames** then auto-released, so a single byte is a tap and repeated bytes hold:

| byte | button | | byte | button |
|---|---|---|---|---|
| `l` | LEFT | | `z` / `o` | O |
| `r` | RIGHT | | `x` | X |
| `u` | UP | | `p` | PAUSE |
| `d` | DOWN | | | |

Upper/lower case both accepted. Unknown bytes ignored. Each received key is logged (`input.serial: r`)
so the receive→map step is verifiable from the serial log alone, independent of the camera.

`O` and `X` are the two action buttons; **which one a cart uses for what is the cart's business.** In
Celeste, `k_jump=4=O` and `k_dash=5=X`, so **`z`/`o` = jump and `x` = dash** (easy to get backwards).

## Frame-exact HITL: telemetry + `INPUT_HOLD_FRAMES` (the Celeste play-test)

Two opt-in build flags turn the serial path into a **deterministic, self-checking** test harness — used
by [`test/playtest/celeste/celeste_playtest.py`](../../test/playtest/celeste/celeste_playtest.py) to drive Celeste through **two full levels
("100 M" → "200 M" → "300 M")** hands-free and confirm each over the wire. Both default off (normal builds
unchanged).

- **`-D TELEMETRY=1`** (app: `firmware/pico-e32-fake08/main.cpp`) — a `GameLoop` variant that prints, each
  Step, `T <frame> <step_us> <draw_us> <x> <y> <room.x> <room.y> <spd.x> <spd.y> <djump>` over UART. The
  `<frame> <step_us> <draw_us>` prefix is **generic** (frame clock + this Step's compute — `step_us` times
  `vm->Step()`, `draw_us` times `host->drawFrame()`; the telemetry `ExecuteLua` poke is *not* counted, so
  they're the cart's real per-frame compute). The player/room tail is Celeste-read via the public
  `Vm::ExecuteLua` (cart sandbox), so it needs **no cart edit and no change to vendored fake-08**. This gives
  the driver ground-truth position (verify a clear via the `room.x/y` change), a frame clock to sync input to,
  **and per-frame timing** — `harness.FpsMeter` aggregates it into min/avg/max *achieved* (`min(target,
  ceiling)`) + *headroom* (`1e6/compute`) fps, so a play-test measures its own frame rate on the board (this
  unifies the old standalone `MEASURE_FPS` timing into the telemetry stream). TX (telemetry) and RX (input)
  share UART0 cleanly.
- **`-D INPUT_HOLD_FRAMES=1`** (this backend) — overrides the default 6-frame auto-release so each byte is
  held **exactly one frame**. Re-send every frame to hold; frame-exact control for an automated,
  frame-synced driver. (6 stays the default for a human typing single keys.)

Why frame-exact: each room needs several frame-precise dashes/jumps a loose open-loop timeline can't land.
The input for each room is *solved* offline against a physics twin
([`test/playtest/celeste/celeste_solver/`](../../test/playtest/celeste/celeste_solver/)) and delivered locked to the telemetry frame
counter; after a room clears, the player respawns at the next room's spawn and the driver re-syncs. (The
driver **drains** the telemetry each loop so the frame counter stays real-time — otherwise the 60 Hz stream
backs up and delivery lags on the later room.) Result: the same clears at the same frames, every run. Build:

```sh
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<board> \
     DEFS='-D CELESTE=1 -D INPUT_BACKEND=serial -D INPUT_HOLD_FRAMES=1 \
           -D FORCE_FLASH_CART=1 -D SHOW_FPS=1 -D TELEMETRY=1'
python3 test/playtest/celeste/celeste_playtest.py <board>   # -> CLEARED 100 M -> 200 M -> 300 M ; PASS (exit 0)
```

The game is ALWAYS flush to the top of the panel (`ESP32Host.cpp`'s `s_oy = 0`), centred horizontally when
the integer upscale is narrower than the glass. The `CENTER_GAME` flag (which used to centre it vertically for
the deck-less serial builds) was removed, so every build — touch, serial play-test, closed-loop — shares one
layout and no build can overlap the game with the touch control-deck's band below. The serial backend also
paints the same on-screen control deck (`board_draw_touch_deck`) as the touch build, so the play-test build
shows the identical button layout — it's cosmetic there (input arrives over the wire; the panel isn't read).

See [`docs/worklog/2026-07-18-celeste-playtest-clear.md`](../worklog/2026-07-18-celeste-playtest-clear.md).

## HITL verification — DONE (2026-07-17)

1. Built `-D INPUT_BACKEND=serial`, flashed to `ttyUSB1`. ✅
2. The default test cart draws a square moved by `btn()` (d-pad) that recolours on O/X, so input is visible
   on the panel. Because the onboard microSD's `.p8` takes cart priority, the demo build also passes
   `-D FORCE_FLASH_CART=1` (a dev/demo flag in the app: ignore any SD cart, run the flash cart). ✅
3. Drove input with pyserial (`b'r'`, `b'd'`, …) on `ttyUSB1` while reading the log: every
   `input.serial: <key>` line appeared, then a **bench-camera frame confirmed the square moved** from
   centre to the bottom-right after `RIGHT×22 + DOWN×22` (frame counter 64 → 148). Full chain
   pyserial → UART0 → scanInput → `btn()` → panel, camera-verified. ✅
4. **Caveat — RESOLVED:** reading `UART0` RX while the console logs on the same UART coexists cleanly
   (keys registered while boot/console logs kept flowing). No fallback to USB-Serial-JTAG needed. ✅

## Backlog

- **IN-1 — seam + serial backend. ✅ DONE (2026-07-17).** The interface, `ESP32Host::scanInput` wiring,
  `input_stub`/`input_serial`, the compile-time switch, and the input-reactive test cart. **HITL-verified:**
  keys driven over `ttyUSB1` registered on-device while the console kept logging; the full
  receive → map → held-mask path confirmed from the serial log.
- **IN-2 — touch (FT6236). ✅ DONE (2026-07-18, PR #11).** All seven zones hardware-verified (serial tap
  test) and confirmed driving real Celeste on the panel. On-screen controls: the PICO-8 screen at the top
  (256×256), a control deck below — a d-pad bottom-left, O/X bottom-right (gamepad diagonal), and a menu
  pill. Approved design ref: [`pico-e32-fake08-touch-ui.html`](pico-e32-fake08-touch-ui.html).
  - **Architecture (mirrors the SD split):** the *board* owns the FT6236 hardware **and orientation** —
    `board_touch_init()` (I²C 0x38, SDA38/SCL39, the `i2c_master` API) and
    `board_touch_read(xs, ys, max) → count` returns up to 2 points already in **display coordinates** (the
    board applies the MIRROR_Y flip, since it owns the display transform). `input_touch.c` owns only the
    *mapping*: point → zone → button bit, OR'd into `input_poll()`. `BOARD_HAS_TOUCH` gates it like
    `BOARD_HAS_SD`; `input_touch.c` declares the two `board_touch_*` symbols `extern` (resolved at app link,
    like `board_lcd_*`).
  - **2-point:** the FT6236 reports two touches, so a direction + O/X register together (move + jump).
  - **Layout zones** (panel 320×480, from the mockup): screen `y 0..256`; deck `y 256..480`. D-pad cross
    centred ~`(92,376)`, arms ~140×50; O ~`(212,414)` r31; X ~`(272,352)` r31; menu pill ~`(160,283)`.
  - **Orientation (resolved)** — the glass is mounted a full **180°** (upside-down AND mirrored), not just
    Y-flipped: `board_touch_read` flips **both** axes to match the display's `offset_rotation=2` (`ROTATE_180`).
    The earlier Y-only guess *looked* right only because the bench camera's own h-mirror masked the X-flip;
    reading the real panel caught it. Fix lives in the board's `ROTATE_180` + `board_touch_read`.
  - **Deck render** — draw the control deck **once** at boot (static; LovyanGFX primitives via the board)
    and move the game render to the top (`drawFrame` OY 112→0). The input path works from zones even before
    the deck is drawn (just not discoverable), so the order is: read → map → then draw.
  - **Verify:** flash `INPUT_BACKEND=touch`; a human taps; the log shows the touched zone and the on-screen
    element reacts (camera-confirmed). On-board, no parts.
- **IN-3 — I²C button expander.** Physical buttons via an expander at addr ≠ 0x38. Parts-blocked; skeleton
  kept so the switch is complete.
- **IN-4 — 30-vs-60 fps + input policy for Gate #4. 🟢 informed (2026-07-18).** Real backends now drive a
  cart, and the pacing model is settled: the host **resumes fake-08's loop at 60 Hz**, and the cart's own
  coroutine sets its logical rate (`_update` → 30 fps, `_update60` → 60 fps) by self-dividing. So there is
  no host-side 30-vs-60 switch — the host always resumes at 60; the cart decides. Celeste (`_update`) runs a
  steady 30 fps with ~6 ms of work (10 ms headroom). Needs `CONFIG_FREERTOS_HZ=1000`. See the
  [fps-resume worklog](../worklog/2026-07-18-fake08-celeste-fps-resume.md). Remaining for Gate #4: **audio**.

- **IN-5 — `fc`-scheduled input backend (frame-exact closed-loop over telemetry). ✅ DONE — written,
  hardware-validated, merged (PR #14, 2026-07-22).** Celeste clears rooms 100→400 M closed-loop on the
  physical board via this backend; the on-device deadline **miss-rate readout** is now wired too (see
  *Acceptance* below). *Why:* a live host-driven closed loop (`live.drive_device`)
  applies each button on whichever `input_poll` catches it — a **~1-frame jittery** latency that is fatal to
  frame-precise carts (measured on the sim: Celeste room (0,0) clears at 0-frame lag, dies at 1, and **any
  jitter fails** — a dash one frame late launches from the wrong pixel). Continuous-control carts (the racer)
  tolerate it; discrete/pixel-precise ones do not. The jitter is the ±1-frame phase mismatch between the
  device's fixed frame clock and the host's async loop — *not* the telemetry cost (already minimized: binary
  + 921600). *Design:* the host tags each command with a **target frame** (the telemetry `fc` it reads, + a
  lead k) and the device applies it when its frame clock **reaches** that `fc` — so host jitter only has to
  **beat a deadline**, and the apply is deterministic. A **core-1 UART task** (APP_CPU is idle; the whole
  runtime is one task on core 0) drains the wire continuously into a lock-free SPSC latch; the apply-by-`fc`
  logic runs in `input_poll` on core 0. The host predicts the target frame with a **lockstep twin** (same
  deterministic policy from spawn, fed the committed commands — proven bit-exact on the sim; VM savestate is
  the parked eris path, so a snapshot won't work). *Wire format* (host→device, 8 B, LE, self-delimiting):
  `0xA6 | target_fc:u32 | mask:u8 | hold:u8 | xor-csum`; hold window `[fc, fc+2·hold)`.
  - *Files:* backend `components/input/input_scheduled.c` (SPSC ring, core-1 `fc_rx` task, apply-by-`fc`,
    `input_sched_stats` counters); seam `input_set_frame(uint32_t)` in `input.h` (main.cpp calls it
    `GetFrameCount()+1` pre-Step — a Step advances the counter by 1, so the fc it emits is `before+1`);
    `CMakeLists.txt` `INPUT_BACKEND=scheduled`. Host + sim twin: `test/playtest/fc_sched.py` (protocol +
    `DeviceScheduler` + `parse_stats`), `test/playtest/test_fc_sched.py`, `celeste/fc_latency.py`.
  - *Sim result:* through the real protocol code, **k=2 lead + binary/921600 → 100% clear** across realistic
    host jitter (k=1 marginal, loose/ASCII needs k=3). See the
    [worklog](../worklog/2026-07-20-celeste-closed-loop-fc-scheduled.md).
  - *Acceptance (hardware): ✅ clear DONE, miss-rate readout WIRED.* Room (0,0) clears closed-loop on the
    board at k=2 (3/3 deterministic, fc=358; chain 100→400 M clears too). The on-device **miss-rate** is now
    streamed: `input_sched_stats(fed, miss, applied)` (deadline miss = a command that arrived after its target
    frame, incl. ring/table drops) is emitted by main.cpp as a periodic `TS <fed> <miss> <applied>` telemetry
    line (~1×/s, scheduled-backend only), which the host drivers parse (`fc_sched.parse_stats`) and report as
    `miss/fed` per lead k — the one number the sim cannot give. Should read ~0 on the tuned k=2 path.
    *Still open:* sweep the reported miss-rate vs lead k and host `low_latency` to confirm it moves as
    predicted (the clear already proves it is ~0 at k=2). Dev/HITL-only — compiles out of production.

- **IN-6 — exit a running cart back to the launcher. 🟡 IN PROGRESS (2026-08-07).**
  - **The gap.** Once a cart launches there is *no way back*. `main.cpp` calls `vm->GameLoop()`, which by design
    never returns; the deck's **MENU** button maps to `INPUT_PAUSE` → `Vm::togglePauseMenu()`, which freezes the
    cart and pauses audio but **renders no options and offers no exit** (upstream leaves it at
    `vm.cpp:1247  //todo: pause menu here, but for now just load bios`). A power-cycle is the only way out, so a
    launcher holding 3145 carts plays exactly one per boot. Found while writing the all-views report.
  - **Why the obvious implementations do not work.** `input_poll()` is **destructive** on the serial backend —
    it drains the UART/USB-JTAG buffer and decrements hold counters — so a second watcher task would *steal*
    input from the VM. And the app cannot check anything itself, because it is blocked inside `GameLoop()`.
    The only per-frame code paths are `ESP32Host::scanInput()` (in the **fake-08 submodule**, so a separate repo
    PR plus a gitlink bump) and `input_poll()` itself (ours).
  - **Approach:** a shared helper in `components/input`, called by each backend from inside its `input_poll()`,
    that measures how long `INPUT_PAUSE` has been held in **wall-clock** (`esp_timer_get_time`) and reboots past
    a threshold (1.2 s, `INPUT_EXIT_HOLD_MS`). Wall-clock, **not** a poll count: the poll cadence is the game
    loop's, and four of `main.cpp`'s five loops are not 60 Hz, so "N consecutive polls" would silently mean a
    different duration in each. A hold also has to be *unbroken* — consecutive held polls more than
    `INPUT_EXIT_MAX_GAP_MS` (250 ms) apart restart the measurement, so a tap followed by a long stall (a
    `carousel_fb_dump()` transfer, a GC pause, a slow SD read) cannot be mistaken for a hold. The launcher
    is ~1.8 s away with the SD already mounted and the radio off, so a restart *is* the return path — no VM
    unwind, no submodule change. Long-press rather than a tap, so it cannot be confused with PICO-8's pause.
  - **Verify:** hold MENU in a running cart on **both** boards and land back in the carousel; a short tap still
    pauses (audio stops) and does not reboot; the launcher's own screens are unaffected (they exit with X).
  - **Not this item:** a real CONTINUE / EXIT overlay, which is what upstream's `//todo` intends. That needs
    rendering inside fake-08's loop and unwinding a loop documented never to return — a deliberate divergence in
    ported code, and it can supersede this later.
