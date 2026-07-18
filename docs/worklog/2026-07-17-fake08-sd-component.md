# 2026-07-17 — SD driver → a configurable, board-agnostic component (`components/sdcard_spi`)

Extracts the fake-08 app's SD mount (`sd_mount.{h,cpp}`, hardcoded to this board's pins) into a **reusable
component** so boards with different SD wiring can share it. The board now **owns its SD wiring** the same
way it owns its display — symmetric with `board_lcd_init`. Follows the SD cart loader
([worklog](2026-07-17-fake08-sd-cart-loader.md), PR #10); folds into that PR so the loader lands as a
proper component rather than app-local code. Design settled via a research + design workflow.

## Why

The pins were hardcoded in an app file. But there are **two boards with different SD wiring**, and the
difference isn't just pin numbers:

- **Board 1** (ILI9488 3.5", this unit) — SD on a **private SPI2** bus (CS=1/MOSI=2/MISO=41/CLK=42),
  disjoint from the i80 LCD. The mount **owns** the bus.
- **Board 2** (ST7701 4.0", later) — SD SPI is **shared with the display's 3-wire SPI**. The display driver
  brings the bus up; the SD must attach a device to the **already-initialized** bus and must **not** free it.

An app-local, owns-the-bus-always driver can't express that second case.

## Design — hybrid, from the design workflow

- **`components/sdcard_spi`** — board-agnostic (`REQUIRES driver esp_driver_spi`; FATFS/sdmmc/sdspi are
  `PRIV_REQUIRES`, private). Never includes a board. Config struct splits **hardware** (board-owned:
  host/pins/`owns_bus`) from **policy** (app-owned: mount point, no-format, max_files).
- **Board provides the wiring** — `bool board_sd_config(sdcard_spi_config_t*)` in `board.cpp`, declared in
  `board.h`, filling only the hardware fields (Approach B: pins stay private in `board.cpp`, beside the LCD
  pins). Grafted with a compile-time `#define BOARD_HAS_SD 1` (Approach A) so a board **without** a slot
  writes zero SD code — its `board.h` omits the macro, and the app's `#if BOARD_HAS_SD` block compiles out.
- **`owns_bus` flag** is the generalization for Board 2: mount runs `spi_bus_initialize` + attaches the
  device; unmount frees the bus — **only when `owns_bus`**. Shared-bus mode skips both and attaches the SD
  as a second device on the live host.

## Two latent bugs fixed in the move

The original `sd_unmount()` always ran `spi_bus_free(SPI2_HOST)` — a **hardcoded literal** (wrong host on
any other board) and **unconditional** (would tear a shared bus out from under the display, and would run
even if the mount failed before the bus came up). The component records the host actually used (`s_host`)
and a runtime `s_bus_owned`, and frees the bus **iff it initialized it**.

## Deviation from the plan

Used a `sdcard_spi_config_default()` **function** (defaults defined once in the `.c`) instead of the
proposed `SDCARD_SPI_CONFIG_DEFAULT()` compound-literal macro — avoids the GNU compound-literal /
designated-initializer edge in `main.cpp` (C++), and keeps the defaults in one place.

## Files

| File | Change |
|---|---|
| `components/sdcard_spi/{sdcard_spi.h,.c,CMakeLists.txt}` | **new** — the component |
| `boards/makerfabs-ili9488-r1/board.h` | `#include "sdcard_spi.h"`, `#define BOARD_HAS_SD 1`, `board_sd_config()` decl |
| `boards/makerfabs-ili9488-r1/board.cpp` | SD pins beside the LCD pins + `board_sd_config()` (SPI2, `owns_bus=true`) |
| `firmware/pico-e32-fake08/main/main.cpp` | `config_default → board_sd_config → mount`, `#if BOARD_HAS_SD`; `SD_MOUNT_POINT` is app policy |
| `firmware/pico-e32-fake08/main/CMakeLists.txt` | drop `sd_mount.cpp` + 4 SD IDF comps; add `sdcard_spi` |
| `firmware/pico-e32-fake08/main/sd_mount.{h,cpp}` | **deleted** |
| `Makefile` | fix a stale comment (`board_pins.h` → `board.{h,cpp}`) |
| `docs/runtime/pico-e32-fake08-sd-cart-report.html` | file-location references → the component |

The fork's `ESP32Host` (`listcarts`/`setCartDirectory`) is **untouched** — it only ever consumed the
`"/sdcard"` string, which is unchanged. `source/` stays byte-identical.

## Verified

- **Clean build** (`make build APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1`, default cart, no DEFS):
  binary 0xe3da0 B, 11% partition free; the component compiles + links with **no new warnings** (the only
  warnings are pre-existing LEDC field-initializer ones in `board.cpp:backlight_on`).
- **Board 1 runtime behavior is identical by construction** — same host, pins, `owns_bus=true`, mount
  policy, and mount point as the hardware-verified loader. Not re-flashed (a behavior-preserving refactor);
  a bench re-verification (SD mount → cart load on the panel) is a cheap confirmation if wanted.

## Follow-up

- **Board 2** shared-bus path is written but untested (no board config exists yet). When that board lands,
  `board_sd_config` sets `owns_bus=false`, host = the display's SPI host, CS=10 — and the ordering
  (`board_lcd_init` before the mount) is already satisfied in `main.cpp`. Validate that LovyanGFX's ST7701
  path brings the host up via a plain `spi_bus_initialize` the SD can attach to.
