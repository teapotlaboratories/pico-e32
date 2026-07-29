# 2026-07-28 — ESP-IDF v5.4.2 → v6.0.2 migration

Goal: bump the vendored ESP-IDF from **v5.4.2** to **v6.0.2** and keep the ESP32-S3 target building,
booting, and running. Motivation: the new Guition **ESP32-P4** board ([[../hardware/pico-e32-guition-jc4880p443c-p4.md]])
needs a modern IDF for its MIPI-DSI stack, so the S3 project should move to the same IDF first — and prove
the move is non-regressive before building the P4 board on it. Decision after verification: **keep v6.**

Everything here is in the working tree, **uncommitted** (commit held: [[../../.ai/AGENTS.md]] forbids commits
during Mon–Fri 09:00–16:59 Pacific, and this is code → feature branch + PR + `/review` to land).

## Why v6.0.2 + the one runtime risk

ESP-IDF v6.0 released 2026-03; **v6.0.2** is the latest patch of the v6.0 line (v6.1 is beta/dev), supported
to 2028. "Mostly compatible with v5.x" but with breaking changes. The one that matters for *this* board: v6
switches the default C library **Newlib → Picolibc**. The board's old boot-loop (the board-recovery section of
`2026-07-20-celeste-closed-loop-fc-scheduled.md`, commit `385a988`) was a *newlib* recursive-stdio-lock
`configASSERT` on the first `printh`, so Picolibc could change that behaviour — flagged as the thing on-device
verification had to check.

Reversible throughout: the submodule was pinned at v5.4.2 (`f5c3654`); revert =
`git -C vendor/esp-idf checkout f5c3654 && git -C vendor/esp-idf submodule update --init --recursive`, and
the v5.4.2 toolchain stays in `vendor/.espressif` (installs are additive). Disk was never a constraint (785 G free).

## The bump

- `git -C vendor/esp-idf fetch --depth 1 origin tag v6.0.2`, checkout, `submodule update --init --recursive`.
- `make install` → v6 toolchain into `vendor/.espressif` (IDF_TOOLS_PATH exported by the Makefile — never `~/`).
- Both clean. `git -C vendor/esp-idf describe --tags` → `v6.0.2`.

## S3 build migration — exactly two changes, both backward-compatible

A clean-configure S3 build (`APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1`, full Celeste DEFS) surfaced two
v6 breakages, fixed in the tree:

1. **Driver-component split.** v6 broke the monolithic `driver` component into per-peripheral
   `esp_driver_uart` / `esp_driver_gpio` / `esp_driver_ledc` / `esp_driver_i2c` / …, and a component must now
   `REQUIRES` the *specific* one that provides its header (bare `REQUIRES driver` no longer re-exports them).
   IDF's own error said it: *"input_scheduled.c includes driver/uart.h, provided by esp_driver_uart."* Fixed:
   - `components/input/CMakeLists.txt` — `driver` → `esp_driver_uart` (serial + scheduled backends' `driver/uart.h`).
   - `components/sdcard_spi/CMakeLists.txt` — `driver` → `esp_driver_gpio` (public `sdcard_spi.h`'s `gpio_num_t`), keeping `esp_driver_spi`.
   - `firmware/pico-e32-fake08/main/CMakeLists.txt` — `driver` → `esp_driver_uart esp_driver_gpio esp_driver_ledc esp_driver_i2c` (main.cpp's uart + board.cpp's ledc/i2c_master/gpio).
   These `esp_driver_*` components exist since **v5.3**, so the fix is valid on v5.4.2 too — no fork, and a v5 revert still builds.

2. **LEDC struct init.** `boards/makerfabs-ili9488-r1/board.cpp` `backlight_on()` used partial designated
   initializers for `ledc_timer_config_t` / `ledc_channel_config_t`. v6 **added fields** (`deconfigure`,
   `intr_type`, `sleep_mode`, `flags`) *and* promotes `-Wmissing-field-initializers` to a hard error. Fixed by
   zero-initializing (`= {}`) then assigning — clean on both v5.4.2 and v6.0.2, and future-field-proof. (These
   were warnings on v5; v6 made them errors.)

**Build result: clean, 0 errors.** And the Newlib→Picolibc switch made the binary **smaller**: app
`0xe0a60` (~899 KB, **12 % partition free**) vs v5's `0xf2470` (~969 KB, 5 % free). More headroom for free.

## On-device verification (S3, CP2104 board) — the real Picolibc test

Flashed the v6 build (identified the board read-only via `/dev/serial/by-id/*CP2104*` → `/dev/ttyUSB1`, so no
esptool probe disturbed the bench camera's tuning). Then `test/playtest/celeste/fc_device.py`:

```
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=/dev/ttyUSB1 \
  DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1 -D INPUT_BACKEND=scheduled -D TELEMETRY=1 -D SHOW_FPS=1 -D CENTER_GAME=1'
python3 test/playtest/celeste/fc_device.py /dev/ttyUSB1 --openloop --to300
#  -> CLEAR at fc=1016 ; fps 9.9/29.8/30.0 ; PASS
```

- **Boots clean — no boot-loop.** The Picolibc risk is cleared; the first-`printh` newlib-assert path is gone
  and the board comes up normally.
- **Celeste runs and clears** 100→200→300 M at **fc=1016** (bit-identical to the v5 result — deterministic),
  fps **29.8** avg (matches v5). Telemetry, the fc-scheduled input backend, and per-frame draw timing all work.
- **Display camera-confirmed** (bench cam `http://192.168.7.135`, de-distorted + uprighted): the panel renders
  the room correctly, backlight on, colours right, green "30 FPS" HUD — which covers the LEDC/backlight change.
  Verified with a still and a video of Celeste clearing 100→300 M on v6 (throwaway `/tmp`, per bench convention).

This satisfies the verify rule end to end: build + wire-level (boot/logic/input/fps) + camera-on-glass (display).

**Compile-only on v6 (honest gap):** the on-device run used `FORCE_FLASH_CART` + `INPUT_BACKEND=scheduled`, so
the **SD-card mount path** and the **touch backend** (`esp_driver_i2c` / `i2c_master` on v6) *compile* on v6
but were **not exercised on hardware** here. Low-risk (standard IDF APIs, and the driver-split for them is
verified at build time), but not run — flag before relying on SD/touch under v6.

## Other apps on v6 — results

Clean-configure build of all four non-fake08 apps on v6 (`scratchpad/build_apps_v6.sh`; esp32 toolchain
installed into `vendor/.espressif` first for bench-cam):

| app | board | v6 result | note |
|---|---|---|---|
| `pico-e32-luabench` | makerfabs (S3) | ✅ **builds clean** | `REQUIRES z8lua esp_timer`, no `driver` — nothing to do |
| `pico-e32-display-test` | makerfabs (S3) | ❌ `board.h: sdcard_spi.h: No such file` | **pre-existing, not v6** — see below |
| `pico-e32-host` | makerfabs (S3) | ❌ same `sdcard_spi.h` | **pre-existing, not v6** — see below |
| `pico-e32-bench-cam` | m5stack (**esp32**) | ✅ **builds clean** (after fix) | v6 driver-split `REQUIRES` — the only real v6 work |

- **display-test + host are broken independently of v6.** Both `#include "board.h"` (display-test `main.c:25`,
  host `host_main.cpp:516`), and `board.h:13` includes `sdcard_spi.h` — added in the **2026-07-17 SD refactor**
  (commit `9596b35`) without adding `sdcard_spi` to those apps' `REQUIRES`. So they've failed on **v5.4.2 too**
  since 2026-07-17. They are the **superseded** Phase-0 de-risking apps (reference-only per the porting rule,
  "not extended"). A one-line fix (`REQUIRES … sdcard_spi`, and — if they compile `board.cpp` — the same
  `esp_driver_*` split as fake08) would revive them, but that's resurrecting dead apps, not a v6 task.
  **Left as-is; flagged for the owner to fix or retire.**
- **bench-cam is the only real v6 work.** `cam_main.c` includes `driver/gpio.h` + `driver/ledc.h` (split out in
  v6) and the **legacy `driver/i2c.h`** for the camera SCCB. Checked: v6 **still ships** legacy i2c
  (`components/driver/i2c/include/driver/i2c.h`), so it wasn't removed — bench-cam just needs the driver-split
  `REQUIRES`. Fixed `firmware/pico-e32-bench-cam/main/CMakeLists.txt`: kept `driver` (legacy i2c.h) and added
  `esp_driver_gpio esp_driver_ledc`. **Confirmed: builds clean on v6** (`0xe2d40` app, 11% free) once dummy
  `WIFI_SSID`/`WIFI_PASS` are passed — the only remaining "error" was the app's intentional `#error` guard for
  missing credentials (by design, never stored in the tree; fires on v5 too), not a v6 issue. No
  `esp32-camera`-component compat problem surfaced.

## Migration complete — the whole surface

Every actively-used app builds on v6.0.2; the two failures are pre-existing breakage in superseded reference
apps, unrelated to v6.

| app | v6 | change |
|---|---|---|
| `pico-e32-fake08` (S3) | ✅ builds **+ boots + runs** (hardware + camera) | driver-split + LEDC zero-init |
| `pico-e32-luabench` (S3) | ✅ builds clean | none |
| `pico-e32-bench-cam` (esp32) | ✅ builds clean | driver-split `REQUIRES` |
| `pico-e32-display-test` (S3) | ❌ pre-broken (2026-07-17, not v6) | superseded — retire or trivially revive |
| `pico-e32-host` (S3) | ❌ pre-broken (2026-07-17, not v6) | superseded — retire or trivially revive |

**Total v6 migration = 5 files, all backward-compatible (valid on v5.4.2 too):**
- `vendor/esp-idf` submodule `f5c3654` (v5.4.2) → `v6.0.2`.
- Driver-component-split `REQUIRES` (v6 split `driver` into `esp_driver_*`): `components/input/CMakeLists.txt`,
  `components/sdcard_spi/CMakeLists.txt`, `firmware/pico-e32-fake08/main/CMakeLists.txt`,
  `firmware/pico-e32-bench-cam/main/CMakeLists.txt`.
- `boards/makerfabs-ili9488-r1/board.cpp` — LEDC config zero-init (v6 added struct fields + `-Werror`).

## Landing plan (commits held until after 17:00 Pacific — work-hours rule)

Code change → feature branch `idf-v6-migration` + PR + `/review` before merge (rebase-merge). The submodule
gitlink bump is part of it. This worklog + its `docs/worklog/html/` render land in the same change. Separately,
the board READMEs + the P4 board doc (doc-only, currently in the same working tree) can go straight to `main`.

## Board state

The S3 CP2104 board currently holds the **v6 Celeste build**, idle after the clear — a known-good state.

## Open / next

- Finish the other-apps migration (above), then a feature branch + PR + `/review`, landed after the work-hours
  window closes. This worklog gets its companion `docs/worklog/html/` render before the change lands.
- The bench-cam legacy-i2c → `i2c_master` migration may be non-trivial (it's the camera's SCCB path).
- Then the P4 board (GP-1…GP-4), now on a modern IDF with proper MIPI-DSI support.
