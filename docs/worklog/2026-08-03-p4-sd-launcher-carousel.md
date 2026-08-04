# 2026-08-03 — SD cart launcher + native cover-art carousel (P4 + S3)

Goal: turn the two handhelds into **cart launchers** — on boot, browse the microSD's PICO-8 library and
launch a cart by selecting it, instead of always running the flash-embedded cart. Built on the P4 first (its
TF slot was assumed absent), then made panel-agnostic and confirmed on the S3.

**Honesty note (process):** this was built *without* a prior TODO in the area backlog and *without* a worklog
kept as I went — both required by [`.ai/AGENTS.md`](../../.ai/AGENTS.md). This worklog is the retroactive record;
the backlog item is now in [`docs/pico-e32-todo.md`](../pico-e32-todo.md) and the P4 board doc. Verified on
real hardware via the **bench camera** throughout (the crisp framebuffer-over-serial path is a dead end here —
see §4).

## TL;DR

- **P4 SD works — the blocker was power, not pins.** The Guition JC4880P443C's TF slot is on the P4 SDMMC
  slot-0 IO_MUX pins (CLK43/CMD44/D0-D3=39/40/41/42, 4-bit), but the card's 3.3 V rail is fed by the **on-chip
  LDO channel VO4**, which must be enabled (`sd_pwr_ctrl_new_on_chip_ldo`, `host.pwr_ctrl_handle`) *before*
  mounting. Without it every init command times out (`sdmmc_init_ocr: send_op_cond (1) returned 0x107`,
  `ESP_ERR_TIMEOUT`) — looks exactly like wrong pins/dead slot. Fix + pins + channel from
  **giltal/RetroESP32-P4** `components/odroid/odroid_sdcard.c` (the same reference that gave GP-6's audio PA-enable).
- **Native cover-art carousel** (`firmware/pico-e32-fake08/main/carousel_launcher.cpp`, new): decodes each
  `.p8.png` label with lodepng, renders a centred cover thumbnail + neighbour peeks + breadcrumb + position bar,
  navigates the SD folder tree, and launches the selected cart into the VM.
- **Runs on both boards from one code path** — layout comes from a new board seam `board_carousel_layout()`, so
  the P4 (480×800, game 384@x48) and S3 (320×480, game 256@x32) each lay out correctly. S3 confirmed live.
- **Folder navigation needed one Host method** — `Host::listdirs()` (esp32) was a `return {}` stub; the whole
  `__cd`/`__listdirs`/`ChangeDirectory` chain was already built in fake-08.
- **Dead end recorded:** framebuffer-screenshot-over-serial (`FB_DUMP`) is unreliable on the P4 USB-Serial-JTAG
  (bulk transfers stall at a *random* 65–685 KB), and lossless compression to dodge it hit a miniz `tdefl`
  wall. The **bench camera is the capture path** for this board.

## 1. P4 SD over SDMMC — the on-chip LDO was the blocker

The P4 board's `board.h` had said "no microSD slot"; the owner confirmed a TF slot exists. Wired it to the P4
SDMMC peripheral and added a **board-owned mount** (`board_sd_mount()` in `boards/guition-jc4880p443c/board.cpp`)
behind a new `BOARD_HAS_SDMMC` flag — distinct from the S3's SPI-SD seam (`BOARD_HAS_SD` + `sdcard_spi`), which
`main.cpp` already handled via `#if BOARD_HAS_SD` / `#elif BOARD_HAS_SDMMC`.

**First attempt failed identically every time:**

```
E sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E vfs_fat_sdmmc: sdmmc_card_init failed (0x107)
W board.p4: SD mount failed (ESP_ERR_TIMEOUT)
```

`0x107` = `ESP_ERR_TIMEOUT` at `send_op_cond` (the ACMD41 command-line handshake). This is the classic
*wrong-pins / no-pull-up / dead-slot* signature. Ruled those out in order:

- **Pins are correct.** A reference for this exact board — `buccaneer-jak/JC4880P443C-P4-to-C6…SD-Card` —
  mounts SD with plain Arduino `SD_MMC.begin("/sdcard", false)`, i.e. the P4 **default IO_MUX pins**
  (CLK43/CMD44/D0-D3=39-42), 4-bit. Matches what I had.
- **Not 4-bit vs 1-bit.** Added a 1-bit fallback; both time out identically at `send_op_cond` — a CMD-line
  failure, independent of data width.
- **It's power.** `giltal/RetroESP32-P4` `components/odroid/odroid_sdcard.c` mounts the same board with the
  **on-chip LDO** as `host.pwr_ctrl_handle` (`sd_pwr_ctrl_new_on_chip_ldo`, `ldo_chan_id = 4`). The P4's SD
  rail is not always-on — it's fed by LDO **VO4** (`sd_pwr_ctrl_by_on_chip_ldo.h`: *"set to 4 [when] LDO_VO4 is
  connected to power the SDMMC IO"*). Arduino's `SD_MMC` brings this up for you; esp-idf does not.

Added the LDO bring-up before mount, with a power-cycle-and-retry (del + recreate the LDO) mirroring the
reference. Result:

```
I board.p4: SD mounted at /sdcard (29820MB, 4-bit, CLK43 CMD44 D0-3=39/40/41/42, LDO VO4)
```

The pin map + LDO channel are recorded in
[`docs/hardware/pico-e32-guition-jc4880p443c-p4.md`](../hardware/pico-e32-guition-jc4880p443c-p4.md).

## 2. Folder navigation — one Host method (`listdirs`)

The card holds carts in genre folders, so the launcher needs to walk subdirectories. The whole machinery for
that was **already in fake-08**: the DefaultCart browser calls `__listdirs()`, which maps to `Vm::GetDirList()`
→ `_host->listdirs()`, and `__cd()` → `Vm::ChangeDirectory()` (handles `..`, absolute, relative) →
`_host->setCartDirectory()` + re-list. The **only** gap was the esp32 `Host::listdirs()`, a
`return {}` stub ("not needed for the first-cart milestone"). Filled it by porting the gcw0 version
(`platform/gcw0/source/ODHost.cpp` `listdirs`): `opendir` the current dir, and for each entry try `opendir` on
its full path — succeeds ⇒ it's a directory (FatFs on esp-idf doesn't populate `dirent.d_type`). See the code
map update in [`docs/runtime/pico-e32-fake08-codemap.md`](../runtime/pico-e32-fake08-codemap.md).

## 3. Native cover-art carousel

A `.p8.png` is a 160×205 PNG whose visible pixels **are** the cart's cover/label (the cart data is
steganographic, low-bits). The launcher decodes that art with **lodepng** (the same decoder fake-08's `Cart`
loader uses — `libs/lodepng` is a public include dir of the `fake08` component) and renders a carousel.

`firmware/pico-e32-fake08/main/carousel_launcher.cpp` (new, ~430 lines):

- **Layout** from `board_carousel_layout()` (§6). Centre thumbnail = a portrait cover tile; breadcrumb (current
  folder name) above; a position bar below; two neighbour "peek" slices at the sides — **all confined to the
  game's on-screen column** so the launcher matches the game's footprint (black borders where the game has
  them). Folder entries render as a folder glyph + name; the `..` back entry as an up-triangle.
- **Input via the shared seam** — polls `input_poll()` (the same `components/input` layer the game uses), so it's
  touch-driven when shipped and can be serial-driven for headless testing without changing the carousel.
- **Repaint is split** to avoid flicker (§5): a full repaint (background + breadcrumb + deck) only on a
  directory change; an item move repaints just the thumbnail + peeks + position bar in place.
- **On select:** a folder does `cd`; a cart returns its path, and `app_main` loads it into the VM as usual — the
  launcher is a pre-game modal.

Wired into `main.cpp` behind `-D LAUNCHER=1`: when the SD mounts, `carousel_launcher_run()` is called instead
of auto-loading a cart, falling back to the flash cart if the SD didn't mount.

**Deliberate divergence (flagged per the porting rules):** this is a *native* launcher, not fake-08's built-in
`DefaultCart` Lua browser. It lives in the app (`firmware/…/main/`), not the `components/fake08` runtime, so it
doesn't touch the 1-to-1 port — but choosing native (full panel resolution + colour, board-driven layout) over
extending DefaultCart (128×128, 16-colour) is a design choice, recorded here.

## 4. Dead end — framebuffer screenshot over serial (`FB_DUMP`)

Tried to get crisp, camera-free captures via the existing `FB_DUMP` path (stream the RGB565 framebuffer over
the console → PNG on the host). It **does not work reliably on the P4 USB-Serial-JTAG**, and the rabbit hole is
worth recording so nobody re-runs it:

- **Raw 768 KB blob stalls at a random point** — observed the device-side write hard-stop at 65 KB, 170 KB,
  632 KB, 685 KB on different attempts (host gets nothing further for seconds). Same family as the known P4
  "USB-JTAG drops long reads" note. The frames that *did* land earlier (the gameplay stills) were the lucky
  subset; the drops were masked because I was grabbing many frames and only needed a few.
- **Chunking made it worse** (consistent stall ~685 KB), not better.
- **Compression to get under the stall floor** hit a miniz `tdefl` wall: this build has `MINIZ_NO_ZLIB_APIS`
  (so `mz_compress2`/`MZ_OK` are gone) and the one-call `tdefl_compress_mem_to_mem` `MZ_MALLOC`s a ~300 KB
  compressor in **internal RAM** (exhausted → returns 0). Fixed by allocating the `tdefl_compressor` in PSRAM
  and using the streaming API — but then chased three more bugs: a comment containing `*/` closed the block
  comment early; the direct-`pOut_buf` mode produced a stream that broke ~4 KB in; a missing
  `TDEFL_COMPUTE_ADLER32` flag. Landed on the callback path (`tdefl_compress_buffer` + a put-buffer callback,
  compressor in PSRAM) which **does** produce a valid ~8 KB zlib blob for a flat carousel screen and transfers
  cleanly — but the game (busy, per-frame redraw) still tears, and the whole thing is fragile.
- **Conclusion:** the bench **camera** is the capture path here (per the HIL-verification rules it's the
  primary evidence anyway). `FB_DUMP` is left in as a dev-only aid, off by default.

## 5. Visual iteration (all camera-verified)

Flash → camera → inspect → adjust, many rounds. Net state of the launcher UI:

- Cover thumbnail is a **portrait tile** sized to fit above the deck with margins (per-board, §6); it letterboxes
  the cover for the centre, crop-fills the side peeks.
- **Folder name lives in the tile** (auto-scaled, wraps on space/hyphen); the bottom name/counter text was
  removed. Breadcrumb (current folder) sits above the thumbnail.
- **Confined to the game column** — earlier the side peeks spilled to the panel edges, making the launcher look
  wider than the game; now nothing renders outside `[game_x, game_x+game_w)`.
- **Flicker fixed** — every navigation used to `board_lcd_fill()` the whole panel + redraw the deck. Now a full
  repaint happens only on a directory change; an item move repaints in place (thumbnail + peeks + bar). Verified
  by recording item-to-item navigation — the deck/background hold steady.
- **Position bar** made visible (its track was ~background-coloured, so only the bright knob showed and looked
  like a stray strip).
- **Capture rig:** the device sits ~4° clockwise on the bench, so stills/video are straightened (−4°) and
  centre-cropped; camera exposure raised to 300 / gain 32 so captures are lit at the source.

## 6. Board-relative layout → S3 port

The carousel constants were hardcoded to the P4's 480×800 panel (e.g. a 384-wide game column) — on the S3's
320×480 panel that gives a **negative** column offset and an oversized thumbnail. Per the owner's steer, moved
the layout into a **per-board seam**: `board_carousel_layout(board_carousel_layout_t*)` declared in each
`board.h` and implemented in each `board.cpp`. The carousel reads it once and derives everything.

| field | P4 (480×800) | S3 (320×480) |
|---|---|---|
| `game_x, game_w` | 48, 384 (128×3) | 32, 256 (128×2) |
| `thumb_w × thumb_h` | 236 × 302 | 148 × 190 |
| `thumb_y` | 46 | 30 |
| `side_w` | 64 | 34 |
| `crumb_y` | 16 | 8 |

`game_x/game_w` match where the fake-08 host actually renders the game (`ESP32Host` `pico_scale`, `s_ox`).

**S3 confirmed live** — same firmware, `BOARD=makerfabs-ili9488-r1`, `-D LAUNCHER=1 -D INPUT_BACKEND=touch`.
It builds (SDMMC/LDO code compiles out; SPI-SD `BOARD_HAS_SD` path used), links (new seam satisfied), and boots:

```
I board-lcd: LCD up via LovyanGFX (320x480 …)
I sdcard_spi: SD mounted at /sdcard   Name: SD32G  Type: SDHC
I carousel: carousel layout: game 32+256, thumb 148x190 @y30, side 34
I board-lcd: FT6236 touch up … touch deck drawn (S3 …)
I carousel: carousel up: 3 entries in /sdcard
```

Camera frame of the S3: the carousel renders correctly on the smaller panel (its SD just has stray folders like
`System Volume Information`, not PICO-8 carts). Evidence frame captured to `/tmp` (throwaway per the frame-storage rule).

## 7. Playtest — carts launch and play, at 30 fps

End-to-end verification on the P4, driven headlessly over serial (a temporary `INPUT_BACKEND=serial` build so the
carousel + game take injected keys), camera-recorded:

- **Racing cart** — carousel → `[Racing]` folder → launch the first cart (**"Don't Fear the Wormhole"**, a
  space racer) → fed accelerate + steer → it responded (accrued distance) and ran through to
  **"GAME OVER — NEW HIGH SCORE! 3.00.18 LI'YRS"**. Launcher → SD cart → playable, confirmed.
- **RPG cart (bonus)** — the first attempt hit `[RPG]` not `[Racing]` (off-by-one: `[RPG]` sorts before
  `[Racing]` — uppercase `P` < lowercase `a` in ASCII, folder names are sorted verbatim). It launched an RPG's
  intro narrative fine — a free confirmation that a completely different cart type also launches and runs.

**On-screen FPS — and a HUD bug it surfaced.** Built with `-D SHOW_FPS=1`. The generic meter
(`ESP32Host::waitForTargetFps`) repainted the HUD **only when the integer value changed**, on the assumption it
"lives outside the game blit region." That holds on the S3 but **not the P4**: the game fills the panel width
(384 @ x48) and blits over the HUD's spot (panel `(8,8)`, i.e. `x8..104`) every frame — so on steady-fps play
the HUD was drawn once, immediately overwritten, and invisible. **Fix:** repaint every tick (the box is tiny;
per-frame cost negligible). With that, the racing cart reads **~30 fps** during gameplay (36 on the menu, where
the coroutine resumes faster) — i.e. the 30 fps cart at its cap. This is a small **`fake08` submodule** change
(`platform/esp32/ESP32Host.cpp`, `SHOW_FPS` block), **off by default** so shipping builds are unaffected.

## 8. Measurements

- **P4 heap after the carousel is up (touch build):** `free heap: 376 KB internal, 31 738 KB PSRAM` (of 32 MB).
  The carousel's PSRAM use (a `thumb_w*thumb_h` scratch ≈ 143 KB + a small decoded-cover LRU) is negligible
  against the 32 MB; internal RAM headroom is healthy. No frame-time meter — the launcher is an event-driven UI
  (repaints on input), not a continuous 30/60 fps loop, so the game-loop frame budget doesn't apply; the
  relevant cost is the per-item repaint (a few localised blits), which reads as instant on the panel.
- **P4 launcher app image:** 936 KB (`0x10000`, hash-verified).
- **SD library on the P4 card:** 3 145 `.p8.png` carts across 19 genre folders (~92 MB), extracted from a
  double-zipped `picowesome v1.5` archive (the 3.9 GB was HTML mirrors/metadata; only the carts were pulled out).

## 9. State + next

- **Boards:**
  - **P4** (`/dev/ttyACM0`, MAC …7E:CA) — currently on the **serial + `SHOW_FPS` demo build** (used to drive the
    playtests headlessly and show the on-screen HUD), **not** the known-good touch shipping build. **Restore
    `-D LAUNCHER=1` (touch, no FB_DUMP/SHOW_FPS) before calling the session closed.** SD has the 3 145-cart library.
  - **S3** (`/dev/ttyUSB0`, CP2104) — `-D LAUNCHER=1 -D INPUT_BACKEND=touch` (known-good). Its SD32G has only
    stray folders; drop `.p8.png` carts on it to browse them.
- **Tree — all uncommitted** (code change ⇒ branch + PR when landing, per the rules; commit **held** until after
  the weekday 9–5 Pacific window):
  - new: `firmware/pico-e32-fake08/main/carousel_launcher.{cpp,h}`
  - `boards/guition-jc4880p443c/board.{h,cpp}` — `board_sd_mount` (SDMMC + LDO), `board_carousel_layout`
  - `boards/makerfabs-ili9488-r1/board.{h,cpp}` — `board_carousel_layout`
  - `firmware/pico-e32-fake08/main/main.cpp` — SDMMC branch, `LAUNCHER` path, carousel + dev-only `FB_DUMP`
  - `firmware/pico-e32-fake08/main/CMakeLists.txt` — forward `LAUNCHER`, add `fatfs sdmmc esp_driver_sdmmc`
  - submodule `components/fake08` — **two** esp32 Host changes: `Host::listdirs()` (folder nav) and the
    `SHOW_FPS` per-tick HUD repaint (§7). Needs a submodule commit + gitlink bump.
  - also present: a dev-only `LCD_FILL` bench-fill-light snippet in `main.cpp`/CMakeLists (from a camera-lighting
    detour) — keep or drop before landing.
- **Next:** decide on landing (branch + PR + `/review`; submodule first, then gitlink); optionally seed the S3's
  card with carts for a full S3 end-to-end; consider whether the launcher should be the default on both boards
  (currently opt-in via `-D LAUNCHER=1`).
