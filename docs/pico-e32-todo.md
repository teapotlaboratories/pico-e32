# pico-e32 — master TODO index

The authoritative backlog root. This file only **points**; the detail lives in the linked
docs (per [`.ai/AGENTS.md`](../.ai/AGENTS.md) → *Plan first*).

- **Plan of record:** [`pico-e32-development-plan.md`](pico-e32-development-plan.md)
- **Evidence base:** [`design-specification/`](design-specification/) (runtime feasibility, silicon decision)
- **Hardware reference:** [`reference/pico-e32-makerfabs-boards.md`](reference/pico-e32-makerfabs-boards.md)
- **Bench camera (HIL verification):** [`hardware/pico-e32-bench-camera.md`](hardware/pico-e32-bench-camera.md) — rig setup + its backlog (`BC-1`…`BC-6`); **`BC-1` (framing/focus) blocks both visual gates**
- **z8lua speedup research:** [`reference/z8lua-speedup-research.md`](reference/z8lua-speedup-research.md) — lever ranking; **profile before optimizing**
- **Bring-up log:** [`worklog/`](worklog/)
- **Firmware:** [`../firmware/`](../firmware/)

## Now — Phase 0 (de-risk on the 3.5" ILI9488 board)

| # | Item | Gate | Where | Status |
|---|------|------|-------|--------|
| B | z8lua interpreter throughput on the LX7 | **#2** ≤ 33 ms/frame of work (30 fps) | [`firmware/pico-e32-luabench`](../firmware/pico-e32-luabench) | ✅ **measured on hardware — passable** (~1.6 M VM inst/s, ~2.5× under target, in the pass window). **Needs `-fjump-tables`** (~3×). See [worklog](worklog/2026-07-14-phase0-gate2-luabench.md) |
| A | ILI9488 scaled blit + FPS | **#1** ≥ 30 fps @ 256² | [`firmware/pico-e32-display-test`](../firmware/pico-e32-display-test) | 🔨 **panel RENDERS at last** — the blocker was the **pin map**: this is Makerfabs' **rev 1**, LCD on **WR=35/DC=36/CS=37** (N16R2 quad PSRAM frees them; their newer N16R8 board moved the LCD to 18/17/46). Driver now **LovyanGFX** (plan §A1's other branch). 16 vertical bars confirmed by camera at 57 px/bar. ⚠️ **Y-flipped** (shape-verified) — open. ❌ **fps UNKNOWN** — the old *288 fps* was DMA into unconnected pins; needs an honest re-measure. See [worklog](worklog/2026-07-16-panel-rev1-pinmap.md) |
| C | Trivial cart end-to-end (minimal `ESP32Host`) | **#3** cart ≥ 30 fps on panel | [`firmware/pico-e32-host`](../firmware/pico-e32-host) | ⚠️ **groundwork done — 161.5 fps** (z8lua + ili9488 + trivial cart). Rendering verified **byte-identical host↔device** (framebuffer checksum). **Panel image still unverified** — bench camera works; blocked on [`BC-1`](hardware/pico-e32-bench-camera.md#open-items) (framing/focus). See [worklog](worklog/2026-07-14-phase0-gate3-host.md), [rig](worklog/2026-07-15-bench-camera.md) |
| B+ | **Gate #2 real-cart confirmation** — Celeste on the S3 | (confirmation + optimization scoping) | [`firmware/pico-e32-luabench`](../firmware/pico-e32-luabench) | ✅ **real Celeste = ~15.8 ms/frame avg** (input-insensitive; per-room 5–40 ms, object-count-driven) → **solid 30 fps** (dense rooms near budget), 60 fps room-dependent; drawing runs on core 1. ⚠️ **Scoped to levels 3–15** — levels 16–30 live in the map's shared-memory region, which wasn't extracted, so *half the game is unbenchmarked*; now unblocked by [`HG-1`](runtime/pico-e32-host-graphics.md). Not a bug (the bench says so), but the average may move. Optimization levers now **deferred** (only needed for 60 fps / heavy carts): globals→locals measured only ~14% on Xtensa; see [research](reference/z8lua-speedup-research.md) + [worklog](worklog/2026-07-14-phase0-gate2-luabench.md) |

## Next — Phase 1 (playable, on the ILI9488)

- **Graphics surface — [`runtime/pico-e32-host-graphics.md`](runtime/pico-e32-host-graphics.md)** (`HG-1`…`HG-7`):
  `spr`/`map`/`print` are still **no-op stubs**, so Celeste's logic runs at frame rate but has never drawn
  a pixel. **Not parts-blocked, and verifiable without the camera** (host frame dump → PNG) — the one
  Phase-1 item that can move right now. `HG-1` ✅ (sprite sheet extracted), `HG-5` ✅ unblocked (font is CC-0 from Lexaloffle). See [worklog](worklog/2026-07-15-host-graphics.md).
- Full `ESP32Host` (fake-08 port): I²C-expander input, I²S audio (external MAX98357A), SD carts. **Parts-blocked.**
- **Gate #4:** a real cart playable ≥ 30 fps with sound + input; set the 30-vs-60 fps policy.
- Parts to buy: I²C GPIO expander + buttons + MAX98357A + speaker + microSD (see plan §7).

## Later — Phase 2+ (the 4.0" ST7701 board)

- Port the host to the ST7701 **RGB** path; run **Gate #5** (RGB drift soak test) before trusting it.
- Enclosure + (only if custom) a PCB with the display that survives Gate #5.

## Open decisions

- Bytecode-precompile strategy (build-time vs load-time) — plan §10.
- Whether the 4" board clears Gate #5 for heavy PSRAM-heap carts — hardware-only answer.
