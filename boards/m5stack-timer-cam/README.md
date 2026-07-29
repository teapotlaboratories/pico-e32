# m5stack-timer-cam

**M5Stack Timer Camera F** — ESP32-D0WDQ6-V3 + OV3660 fisheye sensor, 4 MB flash + 8 MB PSRAM.

> **This is NOT a runtime/handheld target.** It is the **bench-rig capture camera** aimed at the
> panel under test — the hardware-in-the-loop verification eye that photographs the LCD so a change
> can be judged on the real glass without a human watching. Only the `pico-e32-bench-cam` app builds
> for it; the fake-08 runtime, Celeste, etc. do **not** run here.

It appears on the bench as an **FTDI** USB-serial device (`0403:6001`) — distinct from the display
board's CP2104 — so the two are told apart by their USB descriptors (read-only, via
`/dev/serial/by-id/`, so probing never disturbs the camera's tuning).

## Why it's a "board" here

The repo's `BOARD=<name>` mechanism selects a `boards/<name>/` directory for its `sdkconfig.defaults`
and `board.mk`. The bench camera is a different chip (plain **ESP32**, not S3) with its own flash/PSRAM
config and a baud quirk, so it gets its own board dir — even though it carries no display driver.

## Files

- `sdkconfig.defaults` — `CONFIG_IDF_TARGET="esp32"`, 4 MB flash, 240 MHz, PSRAM enabled (camera
  framebuffers need it). No `board.cpp`/`board.h` — there is no panel to drive.
- `board.mk` — **`BAUD := 115200`**. The camera's FTDI bridge cannot sustain idf.py's default
  460800: it syncs at 115200 and then dies on the baud switch with "No serial data received".

## Build

```
make flash APP=pico-e32-bench-cam BOARD=m5stack-timer-cam PORT=<FTDI-port>
```

## See also

- [`docs/hardware/pico-e32-bench-camera.md`](../../docs/hardware/pico-e32-bench-camera.md) — the bench
  rig: setup, de-distort/upright capture tooling, and its backlog.
- The **display board** it observes: [`../makerfabs-ili9488-r1/`](../makerfabs-ili9488-r1/).
