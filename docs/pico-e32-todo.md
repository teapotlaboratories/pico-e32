# pico-e32 — master TODO index

The authoritative backlog root. This file only **points**; the detail lives in the linked
docs (per [`.ai/AGENTS.md`](../.ai/AGENTS.md) → *Plan first*).

- **Plan of record:** [`pico-e32-development-plan.md`](pico-e32-development-plan.md)
- **★ PRIMARY GOAL — port fake-08:** [`runtime/pico-e32-fake08-port.md`](runtime/pico-e32-fake08-port.md) — the runtime is a **port of fake-08** (MIT), not hand-written; replace only its `Host` layer. **Draw-only milestone is unblocked (no parts).** See plan §5.
- **Evidence base:** [`reference/pico-e32-runtime-feasibility.md`](reference/pico-e32-runtime-feasibility.md), [`pico-e32-silicon-decision.md`](reference/pico-e32-silicon-decision.md)
- **Hardware reference:** [`reference/pico-e32-makerfabs-boards.md`](reference/pico-e32-makerfabs-boards.md)
- **Display path (ILI9488 + driver):** [`hardware/pico-e32-display.md`](hardware/pico-e32-display.md) — pin map/bus/orientation status + its backlog (`DP-1`…`DP-7`); **`DP-1` — the repo contradicts itself about whether `esp_lcd` was retried**
- **Bench camera (HIL verification):** [`hardware/pico-e32-bench-camera.md`](hardware/pico-e32-bench-camera.md) — rig setup + its backlog (`BC-1`…`BC-6`); **`BC-1` done — the rig works and caught the Y-flip**
- **z8lua speedup research:** [`reference/z8lua-speedup-research.md`](reference/z8lua-speedup-research.md) — lever ranking; **profile before optimizing**
- **Bring-up log:** [`worklog/`](worklog/)
- **Firmware:** [`../firmware/`](../firmware/)

## Now — Phase 0 (de-risk on the 3.5" ILI9488 board)

| # | Item | Gate | Where | Status |
|---|------|------|-------|--------|
| B | z8lua interpreter throughput on the LX7 | **#2** ≤ 33 ms/frame of work (30 fps) | [`firmware/pico-e32-luabench`](../firmware/pico-e32-luabench) | ✅ **measured on hardware — passable** (~1.6 M VM inst/s, ~2.5× under target, in the pass window). **Needs `-fjump-tables`** (~3×). See [worklog](worklog/2026-07-14-phase0-gate2-luabench.md) |
| A | ILI9488 scaled blit + FPS | **#1** ≥ 30 fps @ 256² | [`firmware/pico-e32-display-test`](../firmware/pico-e32-display-test) | ✅ **GATE #1 PASSES — measured honestly, and the image is correct.** **blit-only 393.0 fps**, **end-to-end 210.6 fps** (expand+scale+blit every frame) vs a **610.4 fps** bus ceiling — 7–13× the gate, frame time 2.54/4.75 ms with ~zero spread. ✅ **Y-flip FIXED** — the glass is mounted mirrored; `.mirror_y = true` → `offset_rotation = 4`; L-pattern verified **upright** by camera. Panel confirmed **live during the timed window** (tearing visible while the palette animates), so this is not another 288. Driver **LovyanGFX** on rev-1 pins (WR=35/DC=36/CS=37). See [worklog](worklog/2026-07-16-yflip-and-gate1-fps.md), [display doc](hardware/pico-e32-display.md) |
| C | Trivial cart end-to-end (minimal `ESP32Host`) | **#3** cart ≥ 30 fps on panel | [`firmware/pico-e32-host`](../firmware/pico-e32-host) | ⚠️ **groundwork done — 161.5 fps** (z8lua + ili9488 + trivial cart). **Panel image now VERIFIED** — the L-pattern cart renders **upright and correct** on the glass (2026-07-16), which was the last thing blocking this; `BC-1`/`BC-3` are done. Two caveats before calling the gate: the 161.5 fps predates the Y-flip fix **and** carries a task-watchdog backtrace inside its 1-second reporting windows (the loop never yields — same bug fixed in Track A), so **re-measure it**. And the framebuffer checksum covers less than it claims — see [`DP-4`](hardware/pico-e32-display.md#open-items). See [worklog](worklog/2026-07-14-phase0-gate3-host.md), [rig](worklog/2026-07-15-bench-camera.md) |
| B+ | **Gate #2 real-cart confirmation** — Celeste on the S3 | (confirmation + optimization scoping) | [`firmware/pico-e32-luabench`](../firmware/pico-e32-luabench) | ✅ **real Celeste = ~15.8 ms/frame avg** (input-insensitive; per-room 5–40 ms, object-count-driven) → **solid 30 fps** (dense rooms near budget), 60 fps room-dependent; drawing runs on core 1. ⚠️ **Scoped to levels 3–15** — levels 16–30 live in the map's shared-memory region, which wasn't extracted, so *half the game is unbenchmarked*; now unblocked by [`HG-1`](runtime/pico-e32-host-graphics.md). Not a bug (the bench says so), but the average may move. Optimization levers now **deferred** (only needed for 60 fps / heavy carts): globals→locals measured only ~14% on Xtensa; see [research](reference/z8lua-speedup-research.md) + [worklog](worklog/2026-07-14-phase0-gate2-luabench.md) |

## Next — Phase 1 (playable, on the ILI9488)

- **Graphics surface — [`runtime/pico-e32-host-graphics.md`](runtime/pico-e32-host-graphics.md)** (`HG-1`…`HG-7`):
  `spr`/`map`/`print` are still **no-op stubs**, so Celeste's logic runs at frame rate but has never drawn
  a pixel. **Not parts-blocked, and verifiable without the camera** (host frame dump → PNG) — the one
  Phase-1 item that can move right now. `HG-1` ✅ (sprite sheet extracted), `HG-5` ✅ unblocked (font is CC-0 from Lexaloffle). See [worklog](worklog/2026-07-15-host-graphics.md).
- **Port fake-08** → `ESP32Host` — the primary runtime goal. **Real Celeste now plays end-to-end** on the
  panel (2026-07-18): draw-only port ✅, SD cart loader ✅, input seam ✅ (**serial + touch/FT6236 both
  HITL-verified** driving Celeste — IN-2 done), and the **fps fixed** (the host resumes fake-08's loop at
  60 Hz — a 30 Hz resume ran 30 fps carts at half speed; `CONFIG_FREERTOS_HZ=1000` for smooth pacing; opt-in
  on-screen FPS HUD). See the [input backlog](runtime/pico-e32-fake08-input.md) and the
  [fps-resume worklog](worklog/2026-07-18-fake08-celeste-fps-resume.md). **The only seam still blocking
  Gate #4 is audio** (MAX98357A, parts-blocked); the physical-button **I²C expander** is also parts-blocked
  (touch needs none). The hand-written `HG-*` draw API is a de-risking harness, **superseded** by fake-08's
  own graphics. Plan in [`runtime/pico-e32-fake08-port.md`](runtime/pico-e32-fake08-port.md).
- **Gate #4:** a real cart playable ≥ 30 fps with sound + input; set the 30-vs-60 fps policy.
- Parts to buy: MAX98357A + speaker (audio); optionally an I²C GPIO expander + buttons for physical input
  (touch via the on-board FT6236 needs none). microSD + slot are on-board. (See plan §7.)

## Later — Phase 2+ (the 4.0" ST7701 board)

- Port the host to the ST7701 **RGB** path; run **Gate #5** (RGB drift soak test) before trusting it.
  **Driver already in hand:** LovyanGFX (already vendored) ships `Bus_RGB` + `Panel_ST7701` and a
  ready-made config for this exact Makerfabs 4" board — same library as the 3.5" i80 driver. Detail +
  the Gate-5 caveat (it's a PSRAM-framebuffer panel by design) in [plan §2b](pico-e32-development-plan.md#2b-verified-hardware--makerfabs-40-st7701-480480-ordered).
- Enclosure + (only if custom) a PCB with the display that survives Gate #5.

## Open decisions

- Bytecode-precompile strategy (build-time vs load-time) — plan §10.
- Whether the 4" board clears Gate #5 for heavy PSRAM-heap carts — hardware-only answer.
