# pico-e32 firmware

ESP32 firmware for the PICO-8 handheld. See the phased plan and go/no-go gates in
[`../docs/pico-e32-development-plan.md`](../docs/pico-e32-development-plan.md), and the
bring-up log in [`../docs/worklog/`](../docs/worklog/).

## Apps

```
firmware/
  pico-e32-fake08/     # THE runtime — a port of fake-08 (the PICO-8 player); boots a cart on the panel
  pico-e32-bench-cam/  # the bench HIL capture camera (M5Stack Timer Cam) — photographs the panel under test
```

The Phase-0 de-risking apps (`pico-e32-luabench` / `-display-test` / `-host`) were **removed** once
superseded by the fake-08 port — their Gate #1/#2/#3 results are preserved in the
[worklogs](../docs/worklog/) (the `2026-07-14` / `-16` phase-0 entries).

Boards live in [`../boards/`](../boards/) (`makerfabs-ili9488-r1` — the 3.5" ILI9488 device;
`m5stack-timer-cam` — the bench camera). Shared code is in [`../components/`](../components/)
(`fake08`, `z8lua`, `input`, `sdcard_spi`, `LovyanGFX`). `BOARD=<name>` selects the pin map + sdkconfig.

## Build

ESP-IDF is vendored at [`../vendor/esp-idf`](../vendor/esp-idf) (git submodule, shallow) with its
toolchain/venv at `vendor/.espressif` (gitignored) — the whole SDK is self-contained under `vendor/`.
First-time setup:

```sh
git submodule update --init --recursive vendor/esp-idf   # populate IDF + its components
make install                                             # one-time: toolchain -> vendor/.espressif
```

From the repo root, `make` sources the vendored IDF and layers the board's sdkconfig under the app's
(override `IDF_PATH` / `IDF_TOOLS_PATH` to share a global install):

```sh
make build         APP=pico-e32-fake08  BOARD=makerfabs-ili9488-r1
make flash-monitor APP=pico-e32-fake08  BOARD=makerfabs-ili9488-r1  PORT=<port>
make help          # list apps + boards
```

Every change is verified on real hardware (or a host/unit test) — a clean build is never enough for
on-device behaviour. A bench camera is aimed at the panel for hardware-in-the-loop display verification;
see [`../docs/hardware/pico-e32-bench-camera.md`](../docs/hardware/pico-e32-bench-camera.md) and
[`../.ai/AGENTS.md`](../.ai/AGENTS.md) → *Verifying changes*.
