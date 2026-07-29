# Guition JC4880P443C-I-W (ESP32-P4) — board bring-up

Supporting a **second, different-architecture** target: the Guition JC4880P443C-I-W, a 4.3″ 480×800
IPS smart-display built on **ESP32-P4** (RISC-V), alongside the existing ESP32-S3 boards. The goal set
with the owner (2026-07-28): **display bring-up first**, and **keep the codebase portable across both
ESP32-S3 and ESP32-P4** — no S3 regression.

Backlog IDs below are `GP-*` (Guition-P4), mirroring the `DP-*` (display) / `BC-*` (bench-cam) style.

## What this board actually is

The model name decodes as **JC-4880-P4-43C** → 480×800, ESP32-**P4**, 4.3″. It is **not** an ESP32-S3
(the assumption the model number invites). Confirmed three ways: esptool on the connected unit reads
`Chip is ESP32-P4 (revision v1.3)`, the vendor spec says P4, and the name decodes to P4.

| | detail | confidence |
|---|---|---|
| MCU | **ESP32-P4** rev v1.3 (RISC-V dual-core ~360 MHz, **no native WiFi/BT**) | ✅ esptool |
| Radio | **ESP32-C6** companion (SDIO/esp-hosted) — for WiFi/BT, off the display path | 🟡 vendor/CNX |
| Flash | **16 MB** | ✅ esptool |
| PSRAM | 32 MB (per vendor spec) | 🟡 verify |
| Display | 4.3″ IPS **480×800**, **ST7701S** controller over **MIPI-DSI** | 🟡 multi-source |
| Touch | **GT911** capacitive, I²C | 🟡 multi-source |
| Audio | **ES8311** codec, I²C (bonus — the S3 project's Gate-4 audio gap) | 🟡 multi-source |
| USB | native USB-Serial-JTAG (`303a:1001`), MAC `80:f1:b2:d3:7e:ca` | ✅ observed |

**Identify the board by a stable label, not a `/dev/tty*` port** (the S3 board and this one renumber on
replug). This P4 is the only native-USB device on the bench, so match it read-only via
`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80F1B2D37ECA-*` (or the MAC `80:f1:b2:d3:7e:ca`)
— it currently maps to `/dev/ttyACM0`, but treat that number as volatile. Flash through the `make` wrapper
(`BOARD=guition-jc4880p443c`), never raw `idf.py`, so the board's `sdkconfig.defaults` (target=esp32p4, PSRAM,
flash) is layered in.

**Unverified, needed before the driver (from the Guition demo/schematic package):** the ST7701S init
sequence (the finicky "no black screen" part), DSI lane count (likely 2) + DPHY clock + panel timing,
the **reset GPIO** and **backlight GPIO/driver** (one source cited reset GPIO23 + backlight GPIO22 via a
Silergy SY7023 PWM driver, but that may be a *different* Guition P4 board — **do not trust it until
confirmed against this board's schematic**), and the GT911 I²C pins/address. This is exactly the class of
"trust the schematic, not a plausible source" caution that cost two days on the S3 rev-1 pin map.

## Portability principle (the hard constraint)

The existing design already supports multiple targets — **`boards/<name>/sdkconfig.defaults` owns
`CONFIG_IDF_TARGET`**, and apps/components draw through the board-agnostic `board.h` contract
(`board_lcd_*`, `board_sd_config`, `board_touch_*`) without naming a chip. The P4 board is a new
`boards/guition-jc4880p443c/` implementing the **same `board.h`** via `esp_lcd_mipi_dsi` + ST7701S, while
the S3 board keeps its LovyanGFX i80 path. Known portability blockers to fix so a P4 build is clean and
S3 is untouched:

- **LovyanGFX is S3-only** (`boards.cmake/esp-idf.cmake` hard-globs `platforms/esp32s3/*.cpp`). It must
  stay **board-scoped** (pulled only by boards that use it) and/or no-op its CMake on non-S3 targets, so a
  P4 build never compiles it. `GP-1`.
- **`make install` is hardcoded `esp32s3`** — add `esp32p4` to the target list. The riscv32 toolchain is
  already installed and IDF 5.4.2 supports esp32p4, so this is small. `GP-1`.
- z8lua's `-fjump-tables -ftree-switch-conversion` is a **generic GCC flag** (portable to RISC-V); the
  Gate-2 throughput number was Xtensa-only and needs re-measuring on P4 (deferred with the runtime).

## Plan (staged, Gate-style) — display first

| # | phase | status |
|---|---|---|
| **GP-1** | **Portability foundation** — add `esp32p4` to `make install`; scope/guard LovyanGFX so a P4 build never sees the S3 glob; confirm the S3 build still passes. No P4 hardware needed. | ☐ |
| **GP-2** | **P4 board scaffold + proof-of-life** — new `boards/guition-jc4880p443c/` (`sdkconfig.defaults` target=esp32p4 + 16 MB flash + PSRAM/DSI, `board.h` 480×800 + `BOARD_HAS_*`, `board.cpp` stub); a minimal display-test build that flashes to `ttyACM0` and logs over serial → proves **P4 build → flash → boot** end to end. | ☐ |
| **GP-3** | **Display bring-up (the core)** — implement `board_lcd_*` via `esp_lcd_mipi_dsi` + ST7701S manual init (lanes, DPHY clock, timing, reset/backlight GPIOs), PSRAM framebuffer, handle the ST7701S-on-DSI rotation quirk (PPA hardware-rotate) so 480×800 renders upright. | ☐ |
| **GP-4** | **Gate-1 verification on the real glass** — fill R/G/B then an L-pattern; confirm **colour + orientation** on the bench camera (re-aimed at the P4 panel). Never trust a checksum — the panel is the oracle. | ☐ |
| — | fake-08 runtime on P4 (RISC-V re-validation of Gate #2 + input + audio) — **DEFERRED**, out of this scope; built on the same `board.h` seam so it drops on later. | — |

## Reference implementations (for the ST7701S init + pins)

- Direct board-support project for the JC4880P443C_I_W (manual ST7701S DSI bring-up, PPA hardware-rotate,
  GT911) — the closest match; get the exact init + pins here.
- [`Nicolai-Electronics/esp32-component-st7701`](https://github.com/Nicolai-Electronics/esp32-component-st7701) — reusable ESP32-P4 ST7701 MIPI-DSI component.
- [`embenix/ESP32-P4-DSI-Support-Hub`](https://github.com/embenix/ESP32-P4-DSI-Support-Hub) — DSI panel boilerplate.
- [esp-idf #16201](https://github.com/espressif/esp-idf/issues/16201) — ST7701S MIPI-DSI rotation on P4 (the quirk).
- [MiniWebRadio #791](https://github.com/schreibfaul1/ESP32-MiniWebRadio/issues/791) — this exact board: ST7701 / GT911 / ES8311, links to the Guition package.
- [Guition ESP32-P4 module page](https://www.guition.com/esp32p4-display-module/esp32p4-display) · [CNX writeup (P4+C6)](https://www.cnx-software.com/2025/08/12/4-3-inch-touch-display-board-features-single-esp32-p4-esp32-c6-module-supports-camera-and-speakers/)

## Open questions for the owner

1. **Guition demo/schematic package** — if it's available locally, it's the authoritative source for the
   ST7701S init sequence + pin map (beats reverse-engineering from third-party repos).
2. **Panel verification** — re-aim the bench camera at the P4 panel, or eyeball the first fills?
