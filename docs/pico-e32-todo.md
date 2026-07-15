# pico-e32 — master TODO index

The authoritative backlog root. This file only **points**; the detail lives in the linked
docs (per [`.ai/AGENTS.md`](../.ai/AGENTS.md) → *Plan first*).

- **Plan of record:** [`pico-e32-development-plan.md`](pico-e32-development-plan.md)
- **Evidence base:** [`design-specification/`](design-specification/) (runtime feasibility, silicon decision)
- **Hardware reference:** [`reference/pico-e32-makerfabs-boards.md`](reference/pico-e32-makerfabs-boards.md)
- **z8lua speedup research:** [`reference/z8lua-speedup-research.md`](reference/z8lua-speedup-research.md) — lever ranking; **profile before optimizing**
- **Bring-up log:** [`worklog/`](worklog/)
- **Firmware:** [`../firmware/`](../firmware/)

## Now — Phase 0 (de-risk on the 3.5" ILI9488 board)

| # | Item | Gate | Where | Status |
|---|------|------|-------|--------|
| B | z8lua interpreter throughput on the LX7 | **#2** ≤ 33 ms/frame of work (30 fps) | [`firmware/pico-e32-luabench`](../firmware/pico-e32-luabench) | ✅ **measured on hardware — passable** (~1.6 M VM inst/s, ~2.5× under target, in the pass window). **Needs `-fjump-tables`** (~3×). See [worklog](worklog/2026-07-14-phase0-gate2-luabench.md) |
| A | ILI9488 i80 scaled blit + FPS | **#1** ≥ 30 fps @ 256² | [`firmware/pico-e32-display-test`](../firmware/pico-e32-display-test) | ⚠️ **FPS measured: 288 fps** (~10× gate, ~94% of i80 bus) — criterion passed. **Visual correctness unverified** (bench camera unavailable). See [worklog](worklog/2026-07-14-phase0-gate1-display.md) |
| C | Trivial cart end-to-end (minimal `ESP32Host`) | **#3** cart ≥ 30 fps on panel | [`firmware/pico-e32-host`](../firmware/pico-e32-host) | ⚠️ **groundwork done — 161.5 fps** (z8lua + ili9488 + trivial cart). Rendering verified **byte-identical host↔device** (framebuffer checksum). **Panel image unverified** (camera). See [worklog](worklog/2026-07-14-phase0-gate3-host.md) |
| B+ | **Gate #2 real-cart confirmation** — Celeste on the S3 | (confirmation + optimization scoping) | [`firmware/pico-e32-luabench`](../firmware/pico-e32-luabench) | ✅ **real Celeste = ~15.8 ms/frame avg** (input-insensitive; per-room 5–40 ms, object-count-driven) → **solid 30 fps** (dense rooms near budget), 60 fps room-dependent; drawing runs on core 1. Optimization levers now **deferred** (only needed for 60 fps / heavy carts): globals→locals measured only ~14% on Xtensa; see [research](reference/z8lua-speedup-research.md) + [worklog](worklog/2026-07-14-phase0-gate2-luabench.md) |

## Next — Phase 1 (playable, on the ILI9488)

- Full `ESP32Host` (fake-08 port): I²C-expander input, I²S audio (external MAX98357A), SD carts.
- **Gate #4:** a real cart playable ≥ 30 fps with sound + input; set the 30-vs-60 fps policy.
- Parts to buy: I²C GPIO expander + buttons + MAX98357A + speaker + microSD (see plan §7).

## Later — Phase 2+ (the 4.0" ST7701 board)

- Port the host to the ST7701 **RGB** path; run **Gate #5** (RGB drift soak test) before trusting it.
- Enclosure + (only if custom) a PCB with the display that survives Gate #5.

## Open decisions

- Bytecode-precompile strategy (build-time vs load-time) — plan §10.
- Whether the 4" board clears Gate #5 for heavy PSRAM-heap carts — hardware-only answer.
