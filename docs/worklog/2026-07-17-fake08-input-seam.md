# 2026-07-17 — fake-08 input seam: a compile-time-selectable backend behind `Host::scanInput()`

Fills fake-08's `Host::scanInput()` — which was a **stub returning zero** — with a real input path built
around a **compile-time-selectable backend seam**, so touch / I2C-expander / serial input can be swapped at
build time behind one interface. Serial lands first because it is the only backend an automated bench can
drive. Backlog and protocol spec: [pico-e32-fake08-input.md](../runtime/pico-e32-fake08-input.md) (IN-1 done).

## Why

The port drew frames but couldn't be *played* — `scanInput()` returned zero, so `btn()`/`btnp()` were
always false. Rather than wire one input device straight into the host, the seam splits **where the mask
comes from** (the device) from **what the host does with it** (edge detection), so every future device drops
in without touching the host.

**Serial first** is the load-bearing choice: an automated bench can't press a button or touch the panel, but
it **can write bytes to the board's CP2104 console UART**. Serial is therefore the only backend the agent can
drive for hardware-in-the-loop (HITL) testing. Touch and I2C drop in behind the same seam later.

## Design — one seam, one compiled backend

- **`components/input`** — a reusable component. `input.h` is the interface: `input_init()` and
  `input_poll()` returning a **held-button mask** (bits `INPUT_LEFT..PAUSE`, ordered to match fake-08's
  `P8_KEY_*`). Backends: `input_stub.c` (default), `input_serial.c`, `input_touch.c` (FT6236 skeleton, IN-2),
  `input_i2c.c` (expander skeleton, IN-3).
- **Exactly one backend compiles.** `CMakeLists` selects it via `INPUT_BACKEND` (default `stub`, so a plain
  build is unchanged): `make build ... DEFS='-D INPUT_BACKEND=serial'`.
- **Edge detection lives in the host, once.** `ESP32Host::scanInput` calls `input_poll()` for the held mask
  and computes the KDown (pressed-this-frame) edge itself as `held & ~prevHeld` — shared across all backends,
  so a backend only ever reports what's currently held. The fake08 component `REQUIRES input`.
- **Serial backend** reads **UART0 RX** — the *same* UART the console logs on — one byte per key:
  `l/r/u/d` for direction, `z` or `o` = O, `x` = X, `p` = pause. Each key is held ~6 frames then
  auto-released (no key-up byte needed).

## Verified on hardware (2026-07-17)

Full chain proven end to end: **pyserial → UART0 → `scanInput` → fake-08 `btn()` → panel**.

- **Console coexistence** — reading UART0 RX while the console logs on UART0 TX was verified to work cleanly:
  sending `r/d/l/u/o/x` over `/dev/ttyUSB1` registered every key in the serial log (`input.serial: <key>`)
  while boot/console logs kept flowing. No fallback to USB-Serial-JTAG was needed.
- **On-panel motion** — with a `FORCE_FLASH_CART` dev flag (ignore any SD cart, run the flash cart) the
  input-reactive test cart ran; driving RIGHT ×22 + DOWN ×22 moved the square from centre to bottom-right,
  confirmed by a bench-camera frame (frame counter 64 → 148).

## Gotchas

- **Serial shares UART0 with the console** by design — that's what makes it bench-drivable — but it means the
  backend reads the same RX the console owns. Verified clean here; keep it in mind before adding an
  interactive console reader.
- **Bench-camera exposure caps ~1200** on the OV3660, so a dark cart wants `exp=1200 gain=0` (gain adds
  grain). Recorded in [pico-e32-bench-camera.md](../hardware/pico-e32-bench-camera.md).

## Files

| File | Change |
|---|---|
| `components/input/{input.h,input_stub.c,input_serial.c,input_touch.c,input_i2c.c,CMakeLists.txt}` | **new** — the component; `CMakeLists` compiles one backend by `INPUT_BACKEND` (default `stub`) |
| fork `platform/esp32/ESP32Host.cpp` | `scanInput` calls `input_poll()`; computes KDown edge `held & ~prevHeld` |
| fake08 component `CMakeLists` | `REQUIRES input` |
| `firmware/pico-e32-fake08/main/main.cpp` | input wiring + `FORCE_FLASH_CART` + the input-reactive test cart |
| `firmware/pico-e32-fake08/main/CMakeLists.txt` | `FORCE_FLASH_CART` opt-in |

## Follow-ups

- **IN-2 — touch (FT6236).** On-board, no parts needed; skeleton already in `input_touch.c`. Next up.
- **IN-3 — I2C expander.** Parts-blocked; skeleton in `input_i2c.c`, drops in behind the same seam.
- Backlog and protocol spec tracked in [pico-e32-fake08-input.md](../runtime/pico-e32-fake08-input.md).
