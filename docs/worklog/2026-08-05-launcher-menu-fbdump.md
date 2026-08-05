# 2026-08-05 — Launcher main menu + framebuffer-over-serial screenshots (both boards)

Goal: front the SD carousel with a proper **main menu** (Games / Settings / About), make every launcher
screen lay itself out from the board (not the S3's hardcoded numbers), and — the surprise win — get the
**framebuffer-screenshot-over-serial** (`FB_DUMP`) path *actually working* on both boards. The
[2026-08-03 SD-launcher worklog](2026-08-03-p4-sd-launcher-carousel.md) recorded `FB_DUMP` as a "dead end"
and the bench camera as the only capture path; that is no longer true — see §3, which supersedes that note.

Backlog: follow-on to `GP-8` (SD cart launcher) in [`docs/pico-e32-todo.md`](../pico-e32-todo.md).

## TL;DR

- **Main menu** (`carousel_launcher.cpp`): boot lands on Games / Settings / About; the selected item sits in a
  snug, centred rounded pill. Games runs the existing SD carousel; **X** at the carousel root returns to the
  menu. Settings has one live knob (**Accent**, recolours the whole UI) plus stubbed Brightness/Volume; About
  shows live build provenance (board, panel, git version, IDF, build date, free PSRAM, maintainer).
- **Layout is board-owned.** `board_carousel_layout()` gained `title_y/title_scale`, `body_y/body_dy`,
  `body_scale` (menu items), and `info_scale` (Settings/About body). The S3 (320×480) and P4 (480×800) each get
  proportional type from one code path — S3 items scale-3, P4 scale-6; S3 info scale-2, P4 scale-3.
- **Centring fix**: `draw_text_centered` was centring on the glyph-cell box including each cell's trailing 1px
  gap, biasing every string ½-cell left; it now centres on the visible ink.
- **`FB_DUMP` works on both boards** — camera-free PNGs streamed over the console. P4: real DPI framebuffer →
  zlib-compressed frame. S3: a host-side **shadow framebuffer** (the GRAM panel can't be read back) → raw frame.
- **Two bugs had masked this as a "dead end"** — console LF→CRLF translation and, on the slow S3 raw dump, the
  **Task Watchdog printing backtraces into the middle of the binary stream**. Both fixed (§3).

## 1. Main menu + board-driven layout

The former whole launcher (the carousel) is now `run_carousel()`, reached from a top-level menu loop that also
dispatches `run_settings()` / `run_about()`. All three screens draw their header, rows, and touch deck from the
board layout, so nothing about the S3's geometry is baked into the launcher any more.

New layout fields (`board_carousel_layout_t`, set in each `boards/<board>/board.cpp`):

| field | S3 | P4 | drives |
|---|---|---|---|
| `title_y` / `title_scale` | 40 / 4 | 96 / 6 | main-menu "PICO-E32" wordmark |
| `body_y` / `body_dy` | 110 / 34 | 200 / 62 | menu row origin + pitch |
| `body_scale` | 3 | 6 | menu item (Games/Settings/About) glyph scale |
| `info_scale` | 2 | 3 | Settings/About body-text scale |

The selected menu item's highlight is now a pill sized to the text (+ padding), centred, rather than a
full-column bar. Settings/About rows are placed just under their header at a text-proportional pitch instead of
borrowing the menu's low, wide `body_y`/`body_dy` (which had left the P4 screens small and gap-heavy).

## 2. Why the camera lied about centring

Every menu string looked a few px left of centre. `draw_text` lays glyphs in 4px cells (3 glyph + 1 gap) ×
scale; `draw_text_centered` was halving `len*4*scale`, i.e. counting the *last* cell's trailing gap as if a
glyph followed it. Now it centres on `(len*4 - 1)*scale` — the actual ink width. This nudged every centred
string (title, subtitle, items, breadcrumbs, section headers) onto the true midline.

## 3. FB_DUMP over serial — the "dead end" was two unrelated bugs

The 2026-08-03 note blamed the P4 USB-JTAG bulk endpoint stalling on big raw writes. That part is real (raw
768 KB stalls at a random 65–685 KB), so the P4 **compresses** first: deflate the framebuffer with fake08's
miniz and stream `FB FB FB FB 'S' 'H' 'T' 'Z' w h clen` + zlib bytes — the blob clears the transfer. The S3
can't use miniz (LovyanGFX bundles a colliding `tdefl_write_image_to_png_file_in_memory`), and its GRAM panel
can't be read back at all (`readRect` returns garbage here), so:

- **Shadow framebuffer** (`boards/makerfabs-ili9488-r1/board.cpp`, `FB_DUMP` only): every `board_lcd_blit`
  tees its region into a full-screen PSRAM buffer; `board_lcd_framebuffer()` hands that to the dump path, which
  streams it raw (`…'S' 'H' 'O' 'T' w h` + RGB565). No compression, no miniz collision.
- **The deck was missing** from S3 shots because the touch deck is drawn with LovyanGFX *vector* calls (to
  dodge the i80 odd-pixel DMA hang), never through `board_lcd_blit`. Fixed by re-rendering the deck into an
  offscreen sprite and copying it into the shadow (`readRectRGB` → `board_lcd_rgb565`, so it's byte-order- and
  colour-order-independent). The panel draw path is unchanged; this only adds a capture-side mirror.

Then two bugs that had made even the framed stream look hopelessly corrupt — both **inject bytes mid-stream**,
which shears every row after the injection point so the image looks progressively rolled:

1. **Console LF→CRLF.** `stdout` maps `\n`→`\r\n`; every `0x0A` data byte became `0x0D 0x0A`. On the P4 the
   `usb_serial_jtag` VFS line-ending mode (`ESP_LINE_ENDINGS_LF`) + `usb_serial_jtag_wait_tx_done` + a trailing
   pad settle it. On the S3 the UART port line-ending setter did **not** take (IDF v6 routes `stdout` via
   `/dev/console` over `/dev/uart/0`), so the S3 bypasses the VFS entirely and writes bytes straight to the FIFO
   with `esp_rom_output_tx_one_char`.
2. **Task Watchdog.** The ~27 s S3 raw dump is a tight loop that starves the idle task; the TWDT fired ~5×
   and printed `Backtrace: …` **into the middle of the pixel data** (found by dumping a synthetic gradient and
   measuring the drift — clean rows except ~5 insertion bursts of `\r\n\r\nBacktrace:`). Fixed by yielding
   (`vTaskDelay(1)`) every 4 KB so the idle task runs and feeds the WDT.

With both fixed the S3 stream is byte-exact (all row deltas = one row, 0 backtraces). Diagnosis method worth
keeping: **point `board_lcd_framebuffer` at a synthetic column-ramp** to isolate transport corruption from
render/shadow bugs — it proved the shadow write was never at fault.

## 4. Verification

Every launcher screen was captured over serial on **both** boards (menu, settings, about, carousel, in-game —
P4 running "13 JUMPS", S3 running a `HELLO.P8` off its card) and eyeballed; the S3 physical panel was also
cross-checked by camera to confirm the shadow is faithful (it is). Both boards were then rebuilt clean
(`make fullclean`, since the `-D` set was shrinking) and re-flashed to the **shipping** config
(`-D LAUNCHER=1 -D INPUT_BACKEND=touch`, no `FB_DUMP`); both boot into the touch-driven launcher (S3: FT6236,
P4: GT911), decks drawn, no faults.

`FB_DUMP` stays `-D`-gated and off by default — it compiles out of shipping builds entirely.
