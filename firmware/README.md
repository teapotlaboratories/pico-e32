# pico-e32 firmware

ESP32-S3 firmware for the PICO-8 handheld. See the phased plan and go/no-go gates in
[`../docs/pico-e32-development-plan.md`](../docs/pico-e32-development-plan.md), and the
bring-up log in [`../docs/worklog/`](../docs/worklog/).

## Layout

```
Makefile                 # (repo root) idf.py wrapper: make build APP=<app> BOARD=<board>
build/<APP>/<BOARD>/      # (repo root) out-of-source build output (gitignored)
boards/
  makerfabs-ili9488/     # 3.5" ILI9488, N16R2 (2 MB quad) — the first device
  makerfabs-st7701-4in/  # 4.0" ST7701, N16R8 (8 MB octal) — later target (Gate #5)
components/              # shared components (root, like rimba's components/)
  z8lua/                 #   PICO-8's fixed-point Lua 5.2 dialect (vendored)
firmware/
  # --- Phase 0: the two project-killers ---
  pico-e32-luabench/     # Gate #2: z8lua interpreter throughput (host-verified)
  pico-e32-display-test/ # Gate #1: ILI9488 i80 scaled blit + FPS
```

## Build

ESP-IDF **v5.4.2** is vendored at [`../vendor/esp-idf`](../vendor/esp-idf) (git submodule,
shallow). First-time setup:

```sh
git submodule update --init --recursive vendor/esp-idf   # populate IDF + its components
make install                                             # one-time: toolchain -> vendor/.espressif
```

The whole SDK is self-contained under `vendor/` — the framework at `vendor/esp-idf` and the
toolchain/venv at `vendor/.espressif` (gitignored). Override `IDF_PATH` / `IDF_TOOLS_PATH` to
share a global install instead.

Then from the repo root, `make` sources the vendored IDF and layers the board's sdkconfig
under the app's (override `IDF_PATH` to use a different install):

```sh
make build         APP=pico-e32-luabench      BOARD=makerfabs-ili9488
make flash-monitor APP=pico-e32-display-test  BOARD=makerfabs-ili9488  PORT=/dev/ttyACM0
make help          # list apps + boards
```

## Status / run order

| App | Gate | Answers | Status |
|-----|------|---------|--------|
| [`pico-e32-luabench/`](pico-e32-luabench/) | **#2** | Is z8lua fast enough on the LX7 for 30 fps? | host-verified **+ compiles for esp32s3**; not yet run on hardware |
| [`pico-e32-display-test/`](pico-e32-display-test/) | **#1** | Can the ILI9488 i80 path push a scaled 128×128 frame at ≥ 30 fps? | **compiles for esp32s3**; not yet run on hardware |

1. **pico-e32-luabench first** (the project-killer): `cd pico-e32-luabench/host && make run` for
   the host sanity check, then `make build APP=pico-e32-luabench` for the real Gate #2 numbers.
2. **pico-e32-display-test**: `make build APP=pico-e32-display-test`, read the FPS, confirm the
   image on the ESP-EYE.

## Verification status

Both apps **compile clean for esp32s3** with the vendored ESP-IDF v5.4.2 (`make build`) —
so the code, component wiring, board sdkconfig layering and `esp_lcd`/z8lua API usage are all
sound against real target headers. The z8lua benchmark is additionally **run-verified on the
host**. What remains is **on-hardware behaviour** — the display image + measured FPS (Gate #1)
and the on-device throughput numbers (Gate #2) — which needs the board. Building for the
target already caught one real bug (a `fix32.h` portability issue on xtensa; see
[`../components/z8lua/LOCAL_PATCHES.md`](../components/z8lua/LOCAL_PATCHES.md)).
