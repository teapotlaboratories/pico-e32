# design-specification

Analysis and feasibility studies behind the [development plan](../pico-e32-development-plan.md).
Each has a Markdown source and a companion HTML render of the same content. The working,
decision-locked plan lives one level up at
[`../pico-e32-development-plan.md`](../pico-e32-development-plan.md) — **start there**; these
are the evidence base.

| Study | Markdown | HTML |
|-------|----------|------|
| **Runtime feasibility** — can a fake-08-style PICO-8 runtime hit playable frame rates on the ESP32-S3? Memory/CPU/display/audio/power budget and the original phased build order. | [pico-e32-runtime-feasibility.md](pico-e32-runtime-feasibility.md) | [.html](pico-e32-runtime-feasibility.html) |
| **Silicon decision (locked 60fps)** — ESP32-P4 (+ ESP32-C6 radio) vs. ESP32-S3-plus-co-processor for full 60fps cart compatibility; why "faster core, not more cores" and why a compute co-processor is ruled out. The S3-misses-the-bar fallback. | [pico-e32-silicon-decision.md](pico-e32-silicon-decision.md) | [.html](pico-e32-silicon-decision.html) |

## Headlines (superseded where they conflict with the development plan)

- **Feasible on the ESP32-S3, target 30 fps first.** PicoPico runs Celeste at ~9 ms/frame on
  a 240 MHz ESP32; the two project-killers to de-risk first are **Lua-on-Xtensa speed** and
  PSRAM bandwidth.
- **Port fake-08's core, don't rewrite** — replace only its host layer with an `ESP32Host`.
- **The first device is the 3.5" ILI9488 board; 2 MB PSRAM is enough to start** — the i80
  path keeps no framebuffer in PSRAM, so ~2 MB is free for the Lua heap.
- **Display interface > screen size** — the i80 board is immune to PSRAM-contention drift; the
  4" RGB board must clear a hardware drift soak test (Gate #5) first.
- **If locked 60 fps on *every* cart is a hard requirement**, the fallback is the ESP32-P4 +
  ESP32-C6; the honest non-MCU floor is a Raspberry Pi Zero 2 W with the official binary.

## Caveats

Key figures are cited inline. Treat derived/projected numbers (e.g. the P4's ~1.7–2.2× Lua
speedup) as estimates to be replaced with on-device benchmarks — each study calls out its own
measured-vs-derived-vs-speculative split.
