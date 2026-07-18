# 2026-07-18 — Celeste plays, but "fps is horrible": the 60 Hz-resume fix

First real cart end-to-end: the fake-08 port (`firmware/pico-e32-fake08`) boots **real Celeste** from an
embedded flash cart and renders it through fake-08's own `Vm::GameLoop` — full `spr`/`map`/`print`, not
the draw-only test cart. Touch input (IN-2) and serial input (IN-1) both drive it. Then the owner played
it and said **"fps is horrible."** This log is how that was diagnosed and fixed.

**Status:** ✅ fixed and verified (60 Hz resume; Celeste at correct 30 fps motion; on-screen FPS HUD added).

---

## Bench state

| | |
|---|---|
| board under test | `/dev/ttyUSB0` — CP2104 (Makerfabs ILI9488 rev-1, N16R2) |
| bench camera | `/dev/ttyUSB1` — FTDI (M5 Timer Cam F), serving `http://192.168.7.135` |
| app | `pico-e32-fake08`, `DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1'` + an input backend |
| identify ports | `udevadm` by USB chip — the `ttyUSB*` numbering is not stable (it swapped again this session) |

## Measure first — the hardware is not the bottleneck

Built with the app's `MEASURE_FPS` loop (times `Step()` vs `drawFrame()` in isolation, no pacing):

| Metric | Value |
|---|---|
| `Step()` (VM `_update`+`_draw`) | **2.56 ms** |
| `drawFrame()` (unpack → RGB565 → 2× → strip-blit) | **3.61 ms** |
| Total work | **~6.2 ms/frame → 162 fps capable** |

So "slow" was never the S3 struggling. It was **two separate pacing bugs**.

## Bug 1 — pacing quantization (judder)

The 30 fps limiter (`Host::waitForTargetFps`) sleeps with `vTaskDelay`, which rounds to whole FreeRTOS
ticks. The tick was the IDF default **100 Hz = 10 ms**, so a 33.3 ms target quantized to ±10 ms — frames
landed at 20/30/40 ms and juddered even though the *average* was ~30 fps.

**Fix:** `CONFIG_FREERTOS_HZ=1000` (1 ms pacing) in `firmware/pico-e32-fake08/sdkconfig.defaults`.

## Bug 2 — half-speed (the real "slow")

The instrumented delivered rate came out a rock-steady **30.0 fps** after Bug 1 — yet it still felt slow.
The cause is in fake-08's game-loop coroutine (`p8GlobalLuaFunctions.h`, `__z8_run_cart` glue):

```lua
while true do
  if _update60 then _update_buttons(); _update60()
  else              yield();  _update_buttons(); _update()   -- extra yield for 30 fps
  end
  if _draw then _draw(); flip()  -- flip() yields
  else               yield() end
end
```

The coroutine is designed to be **resumed at 60 Hz**:

| cart | resumes per drawn frame | logical fps at 60 Hz host |
|---|---|---|
| `_update60` | 1 | 60 |
| `_update` (Celeste) | **2** (the extra `yield()`) | **30** |

Our `ESP32Host` paced the loop at **30 Hz** (`app_main` called `setTargetFps(30)`; the host default was
also `1000000/30`). So Celeste got 30 resumes/s ÷ 2 = **15 fps of actual motion — half speed.** Upstream
fake-08's own `source/main.cpp` uses `setTargetFps(60)`; we had diverged.

**Fix:** resume at 60 Hz — `host->setTargetFps(60)` in the app (and the `ESP32Host` default → `1000000/60`).
The host clock (60) and the cart's logical rate (30 for `_update` carts) are different things; the host
always resumes at 60 and the coroutine self-divides. This covers **every** PICO-8 cart — the platform only
defines 30 (`_update`) and 60 (`_update60`); there is no other rate.

## Verification (serial-driven, so it needs no human at the glass)

Drove the game over the IN-1 serial backend (inject button bytes on UART0) while reading the delivered rate:

| | before | after |
|---|---|---|
| loop resume rate | 30.0 fps / 33.3 ms | **60.0 fps / 16.67 ms** |
| Celeste motion | 15 fps (half) | **30 fps (correct)** |
| `scanInput` cost | — | 0.03 ms (serial) / 0.67 ms (touch) |

Camera frames confirmed real gameplay (the "100 M" room, Madeline responding to injected input). A rotated
MJPEG capture (`/stream`) recorded a clean gameplay clip.

## On-screen FPS HUD (`-D SHOW_FPS=1`)

Added an opt-in HUD: the loop rate painted in the panel's **right letterbox** (x ≥ 288), which the centred
256 px game blit never overwrites — so it persists without a per-frame redraw and never covers gameplay.
`board_lcd_draw_fps()` lives in the board (LovyanGFX); `ESP32Host` calls it when the value changes, gated
by `SHOW_FPS` in `components/fake08/CMakeLists.txt`. Reads **60** when healthy; drops if a room ever
exceeds the 16.6 ms budget, so it doubles as a live perf meter.

## Lesson

**A steady average fps can still be half-speed.** fake-08's loop is a 60 Hz-resume coroutine that
self-divides 30 fps carts internally — the host must resume at 60, never at the cart's logical rate.
Pacing at the cart's rate is the exact bug here. Measure the *loop resume rate* and the *game motion*
separately; they are not the same number.
