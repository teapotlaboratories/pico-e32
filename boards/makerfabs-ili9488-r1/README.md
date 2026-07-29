# makerfabs-ili9488-r1

**Makerfabs "ESP32-S3 Parallel TFT with Touch (ILI9488)", 3.5″, 320×480 — FIRST REVISION.**

This is the project's **primary runtime board** and the Makefile default
(`BOARD ?= makerfabs-ili9488-r1`). The fake-08 PICO-8 runtime, Celeste, the Pico Racer, and the
fc-scheduled input path all run here. Selecting it (`BOARD=makerfabs-ili9488-r1`) pulls in this
directory's `board.{h,cpp}` (the pin map + LovyanGFX driver) and `sdkconfig.defaults` (the S3 /
PSRAM config).

> ⚠️ **This is the *first revision* (N16R2), and its LCD pins differ from the vendor's current board.**
> ESP32-S3 octal PSRAM occupies GPIO 35/36/37, so when Makerfabs moved to the N16R8 part they
> relocated the LCD. Both pin maps are published by the vendor, for different boards, with nothing
> marking which is which — and the *wrong* map leaves the panel backlit-white while DMA completes,
> checksums match, and every diagnostic reports success. **Check the PSRAM part, not just the vendor.**
> It cost two days. See `board.cpp`'s header and
> [`docs/worklog/2026-07-16-panel-rev1-pinmap.md`](../../docs/worklog/2026-07-16-panel-rev1-pinmap.md).
>
> | revision | PSRAM | LCD WR / DC / CS |
> |---|---|---|
> | **rev 1 (this unit)** | N16R2 · 2 MB quad | **35 / 36 / 37** |
> | current rev | N16R8 · 8 MB octal | 18 / 17 / 46 |

## Capabilities

| | detail |
|---|---|
| **Display** | ILI9488, 16-bit i80 parallel, driven by **LovyanGFX** (`Bus_Parallel16` + `Panel_ILI9488`) at a 40 MHz WR strobe. `BOARD_HAS` — always. |
| **microSD** | `BOARD_HAS_SD` — a **private SPI2 bus** (no pin overlap with the i80 LCD, `owns_bus=true`). Cart loader. |
| **Touch** | `BOARD_HAS_TOUCH` — onboard **FT6236** capacitive panel, I²C, 2-point. |
| **Orientation** | The glass is mounted a full **180°** (`ROTATE_180`) — the panel is physically upside-down; `board_touch_read` flips both axes to match. |

## Pin map (rev 1)

| function | pins |
|---|---|
| LCD control | WR **35**, DC/RS **36**, CS **37**, RD 48 (held high), BL **45** (PWM via LEDC) |
| LCD data D0..D15 | 47, 21, 14, 13, 12, 11, 10, 9, 3, 8, 16, 15, 7, 6, 5, 4 *(identical on both revisions)* |
| microSD (SPI2) | CS **1**, MOSI **2**, MISO **41**, CLK **42** |
| Touch (FT6236, I²C0) | SDA **38**, SCL **39**, addr **0x38** (INT/RST are NC → polled) |

The pins live in exactly one place — `board.cpp` — and geometry/capability macros in `board.h`.

## Config (`sdkconfig.defaults`)

ESP32-S3, module **ESP32-S3-WROOM-1-N16R2** (16 MB flash + 2 MB **quad** PSRAM), 240 MHz,
`SPIRAM` quad @ 80 MHz. Apps layer their own `sdkconfig.defaults` on top (e.g.
`firmware/pico-e32-fake08/` pins the assertions/console settings the demo needs).

## Files

- `board.cpp` — the LovyanGFX bring-up, pin map, orientation/byte-order, SD + touch wiring. The
  header comment is the authoritative pin-map warning and the source-of-record citation.
- `board.h` — the board-agnostic contract apps draw through (`board_lcd_*`, `board_sd_config`,
  `board_touch_*`) plus geometry/capability macros.
- `sdkconfig.defaults` — target + PSRAM/flash config for this board.

## Build / flash

```
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=/dev/ttyUSB0 \
  DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1 -D INPUT_BACKEND=scheduled -D TELEMETRY=1 -D SHOW_FPS=1'
```

The board's USB-serial bridge is a **Silicon Labs CP2104** (`10c4:ea60`) — this is the port to flash.

## Why LovyanGFX, not `esp_lcd`

An `esp_lcd` i80 backend was built and bench-tested (2026-07-16): faster (590 fps vs 393, zero-copy
DMA) but its colour is broken on this board in a way byte-swapping does not fix. Removed; LovyanGFX
owns the panel. Details in
[`docs/worklog/2026-07-16-esp-lcd-vs-lovyangfx.md`](../../docs/worklog/2026-07-16-esp-lcd-vs-lovyangfx.md).

## See also

- [`docs/reference/pico-e32-makerfabs-boards.md`](../../docs/reference/pico-e32-makerfabs-boards.md) — hardware reference.
- [`docs/hardware/pico-e32-display.md`](../../docs/hardware/pico-e32-display.md) — display bring-up + backlog.
- The repo `Makefile` — how `BOARD=<name>` selects the pin map + `sdkconfig`.
