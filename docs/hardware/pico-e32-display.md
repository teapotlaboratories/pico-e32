# Display path (ILI9488, 16-bit parallel) — status + backlog

The 3.5" ILI9488 panel and the driver that feeds it. The board owns its own display:
[`boards/makerfabs-ili9488-r1/board.cpp`](../../boards/makerfabs-ili9488-r1/board.cpp) (LovyanGFX
`Bus_Parallel16` + `Panel_ILI9488`, pins/orientation/byte-order baked in) behind the board-agnostic
API in [`board.h`](../../boards/makerfabs-ili9488-r1/board.h) (`board_lcd_*`). Apps
([`firmware/pico-e32-display-test`](../../firmware/pico-e32-display-test), the Gate #1 harness, and
`pico-e32-host`) call that API and compile the board's `board.cpp` into `main`. *(There was briefly a
reusable `components/ili9488` wrapper with two selectable backends; with one board and one backend it
was removed 2026-07-16 — see [worklog](../worklog/2026-07-16-esp-lcd-vs-lovyangfx.md).)*

The backlog for this area (per [`.ai/AGENTS.md`](../../.ai/AGENTS.md) → *Plan first*); reachable from the
master index [`docs/pico-e32-todo.md`](../pico-e32-todo.md). The rig that verifies all of it is the
[bench camera](pico-e32-bench-camera.md).

## Status

| | |
|---|---|
| **Pin map** | rev 1: WR=35 DC=36 CS=37 BL=45 RD=48, D0–D15 = 47,21,14,13,12,11,10,9,3,8,16,15,7,6,5,4 |
| **Bus** | LovyanGFX `Bus_Parallel16`, **40.000 MHz** (240 MHz ÷ 2 ÷ 3, exact), `dlen_16bit` |
| **Pixel format** | COLMOD `0x55` — RGB565, **1 bus cycle/px** (the ILI9488's RGB666 penalty is SPI-only) |
| **Orientation** | glass mounted Y-mirrored → `.mirror_y = true` → `offset_rotation = 4` → MADCTL `0x98`. ✅ verified upright |
| **Gate #1** | ✅ **PASS** — blit-only **393.0 fps**, end-to-end **210.6 fps**, ceiling 610.4 fps |

Detail + evidence: [worklog 2026-07-16 — Y-flip and Gate #1 fps](../worklog/2026-07-16-yflip-and-gate1-fps.md),
[worklog 2026-07-16 — the rev-1 pin map](../worklog/2026-07-16-panel-rev1-pinmap.md).

## Open items

| # | Item | Why | Verified by | Status |
|---|------|-----|-------------|--------|
| **DP-1** | ~~**Resolve the `esp_lcd` contradiction**~~ **RESOLVED** | The worklog side was right: esp_lcd was **never tested on the correct pins** before 2026-07-16; the code comments (in the then-existing `ili9488` component) that claimed "retried … gets colour wrong: red/yellow → blue/green" as a *measured* result were overclaims inherited from wrong-pin runs. Bench-tested since: colour **is** broken, but **not** the way claimed (see DP-2). The offending comments went away with the component. | Bench test done | ✅ **done** — [worklog](../worklog/2026-07-16-esp-lcd-vs-lovyangfx.md) |
| **DP-2** | **esp_lcd i80 colour was broken on this board (backend since removed)** | An esp_lcd i80 backend was built and bench-tested (2026-07-16). It was **1.5× faster** (590 vs 393 fps, **true zero-copy DMA** — the `DP-3` win, realised) BUT colour was wrong: **every bright fill (R/G/B alike) rendered the same teal, and toggling the byte pre-swap changed nothing** — so *not* a byte-order bug (a swap would change the hue). Brightness/structure survived; black stayed black. Two hardware swap routes failed: `flags.swap_color_bytes` shreds the 16-bit frame; crossing `data_gpio_nums` (to mimic LovyanGFX's `Bus_Parallel16.cpp:160-166` crossover) scrambles the shared 8-bit init commands. **Working theory:** this board's data-bus wiring needs LovyanGFX's exact crossover, entangled with esp_lcd's command path. **Decision: LovyanGFX only; the esp_lcd backend was REMOVED** (Gate #1 passes 13× over, so no pressure, and it was a real unsolved blocker). The findings are preserved in the [worklog](../worklog/2026-07-16-esp-lcd-vs-lovyangfx.md); re-add from git (`e0a21cc` had a hand-rolled i80 driver) if revisited. | Logic analyzer on D0-D15/WR/DC, or owner board-wiring insight | 📋 **parked — needs a logic analyzer or owner insight; NOT blocking** |
| **DP-3** | **`board_lcd_blit` never uses DMA** — switch to `pushImageDMA` | `pushImage`'s `use_dma` parameter **defaults to `false`** (`LGFXBase.hpp:449`), so every blit `memcpy`s through a **256-byte** double buffer (`Bus_Parallel16.hpp:117`, `CACHE_SIZE = 256`) and spin-waits on the previous chunk before each copy (`Bus_Parallel16.cpp:415`) — the CPU copy never overlaps the transfer. A 131,072-byte frame becomes **512 serialised round-trips + 128 KB of memcpy**, versus one descriptor chain and one `LCD_CAM_LCD_START`. This is most of the gap between the measured 393 fps and the 610.4 fps ceiling: overhead is 1.06 ms/frame ÷ 512 chunks ≈ **2.07 µs/chunk (~498 CPU cycles)**, and the arithmetic closes to the measurement exactly. `pushImageDMA` (`LGFXBase.hpp:436-439`) takes the zero-copy path; both harness buffers are already `MALLOC_CAP_DMA`. ⚠️ **Not free:** DMA reads the caller's buffer directly, so the harness's rebuild-in-place (`main.c`) becomes a genuine **tearing race** unless the per-blit drain is kept. The two changes are individually safe and jointly unsafe. | fps before/after, plus a capture showing no tearing/corruption | 📋 open — **optimisation, NOT needed for Gate #1** (which passes 13× over) |
| **DP-4** | **The framebuffer checksum's coverage is oversold** | `host_main.cpp:8-11` calls `fb_hash()` "the camera-independent correctness check". It hashes the **indexed** `fb[128*128]` only — **everything from palette expansion to the wire is outside its coverage**, which is exactly the stretch the two-day pin-map bug lived in. Worse, `host_open` hardcodes the RGB565 byte swap unconditionally while `board_lcd_blit` uses the board's `SWAP_COLOR_BYTES`; they agree only because the board says `true`. Set it `false` and the panel gets double-swapped colours **while the checksum stays identical**. Fix: have `host_open` call `board_lcd_rgb565()` so there is one definition of the swap policy — which also kills the third copy of `PAL888` (`host_main.cpp:25`, `main.c:45`, `tools/p8_png.py:19`, the last already carrying a "must stay identical" comment). | Flip the board's `SWAP_COLOR_BYTES`, confirm the checksum *changes* or the colours don't | 📋 open |
| **DP-5** | **Negative control: prove the fps counter is blind to a missing panel** | Gate #1's own worklog predicted the 288 fps failure **in writing** ("a broken panel init could still clock DMA at 288 fps into a blank screen") and it was written off as hypothetical — then cost two days. Run it as an experiment: flash the identical binary with WR or CS deliberately misconfigured, confirm the **fps is unchanged** while the camera goes **blank**. That converts the void-mode from a warning into a documented, reproducible fact. | fps identical ±1%, capture blank | 📋 open — cheap, high value |
| **DP-6** | **`board_lcd_fill()` — slow, redundant, one latent OOB** | 480 separate `pushImage` calls, each a full transaction (~1,440 chunk round-trips) for one clear. `s_lcd->fillScreen()` is one `setWindow` + one repeat-write. It is also **redundant**: LovyanGFX's `init_impl(use_reset, use_clear)` already clears (`LGFXBase.cpp:3636-3639`) before both apps clear again. Latent bug: the guard in `board.cpp` clamps the **fill** to `line[480]` but still blits `w` entries — any board with `H_RES > 480` reads past the buffer. That is the "guard that produces wrong output instead of failing" pattern. Only called once at boot, so **zero fps impact**. | Unit-testable; or capture a clear | 🟡 low priority |
| **DP-7** | **`bus_shared = true` is inert, wrongly justified, and diverges from the source of record** | `board.cpp` says `/* the microSD shares this bus */`. **It doesn't** — microSD is CS=1/MOSI=2/MISO=41/CLK=42 (`docs/reference/pico-e32-makerfabs-boards.md:29`), disjoint from the LCD's pins. The flag costs **nothing** per transaction (its only real consumer is `prepareTmpTransaction`, called solely from `drawBmp`/`drawJpg`/`drawPng`/font-from-file paths), so it is **not** distorting any fps number. But the rev-1 source of record sets `bus_shared = false`, and `board.cpp` carries the config that "comes from the REV-1 source of record" — so this is an undocumented deviation of exactly the kind that file's own banner warns about. Also fix `docs/reference/pico-e32-makerfabs-boards.md:16`'s "(bus shared with the LCD)" for the 3.5" board, which looks carried over from the 4" RGB board's entry at `:92` where it is genuinely true. | Code reading; no hardware needed | 🟡 low priority |

> **The trap this area keeps setting.** The knobs that *look* like the orientation/colour controls in
> `board.cpp` and are not: `SWAP_COLOR_BYTES` decides **which side** does the byte swap, not whether
> colours are right — flipping it to chase a colour bug changes nothing visible and quietly costs fps.
> (The old config struct also carried a dead `.madctl` byte and an advisory `.max_transfer_bytes`; both
> are gone now that the board owns the driver directly.) The live knobs are `MIRROR_Y` for orientation
> and `p.invert` / `p.rgb_order` for colour.
