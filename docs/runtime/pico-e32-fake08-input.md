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
- **IN-2 — touch (FT6236).** Bring up the I²C touch controller; map screen zones (d-pad + O/X) → mask; the
  board supplies its touch wiring via a `board_input_config()` (I²C pins/addr), symmetric with
  `board_sd_config`. On-board, no parts — verified by a human tap + camera.
- **IN-3 — I²C button expander.** Physical buttons via an expander at addr ≠ 0x38. Parts-blocked; skeleton
  kept so the switch is complete.
- **IN-4 — 30-vs-60 fps + input policy for Gate #4.** Once a real backend drives a cart, set the frame policy.
