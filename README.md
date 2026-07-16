# pico-e32

An open-source **PICO-8 handheld** built around the **ESP32-S3**.

The goal is a pocket-sized, DIY console for running [PICO-8](https://www.lexaloffle.com/pico-8.php)
carts on affordable ESP32-S3 hardware, via a clean-room runtime (a fake-08 port on
[z8lua](https://github.com/jtothebell/z8lua), PICO-8's fixed-point Lua dialect).

## Current direction

The plan of record is [`docs/pico-e32-development-plan.md`](docs/pico-e32-development-plan.md)
(verified against primary sources). In short:

- **First device:** Makerfabs **3.5" ILI9488** board (ESP32-S3-N16R2, 2 MB PSRAM, 16-bit i80
  parallel). Its 2 MB is enough to start — the i80 panel has its own GRAM, so no framebuffer
  lives in PSRAM.
- **Runtime:** port **fake-08**'s VM; write one `ESP32Host` (display / audio / input / SD /
  timing). Target a **30 fps** baseline.
- **Later:** the Makerfabs **4.0" ST7701** board (8 MB octal, onboard audio + charging) is a
  candidate for the finished handheld — *if* it clears a hardware RGB-drift soak test
  (interface class matters more than screen size).
- **Phase 0** de-risks the two project-killers before any PCB: z8lua throughput (Gate #2) and
  the i80 blit frame rate (Gate #1). See [`firmware/`](firmware/).

## Repository layout

```
.ai/                    # guidance for AI coding agents working in this repo
boards/                 # per-board sdkconfig.defaults (owns target + PSRAM)
  makerfabs-ili9488-r1/     #   3.5" ILI9488, N16R2 (2 MB) — the first device
  makerfabs-st7701-4in/  #   4.0" ST7701, N16R8 (8 MB) — later target
components/             # shared components
  z8lua/                 #   vendored PICO-8 fixed-point Lua
docs/
  pico-e32-todo.md       # master backlog index
  pico-e32-development-plan.md(.html)   # the plan of record + visual render
  design-specification/  # feasibility + silicon-decision studies
  reference/             # verified board pinouts
  worklog/               # bring-up log (+ html/)
firmware/               # ESP-IDF apps (build via the top-level Makefile)
  pico-e32-luabench/     #   Gate #2 — z8lua throughput (host-verified)
  pico-e32-display-test/ #   Gate #1 — ILI9488 i80 scaled blit + FPS
Makefile                # make build APP=<app> BOARD=<board>
```

## Build

ESP-IDF v5.1+ (export `IDF_PATH`). From the repo root:

```sh
make build flash monitor APP=pico-e32-luabench BOARD=makerfabs-ili9488-r1
make help    # list apps + boards
```

The z8lua benchmark also builds natively for a quick sanity check:
`cd firmware/pico-e32-luabench/host && make run`.

## Status

🚧 Planning + Phase-0 bring-up. The runtime and display approach are decided; the two
go/no-go gates are being exercised on hardware. Nothing is committed to a PCB yet.

## License

TBD.
