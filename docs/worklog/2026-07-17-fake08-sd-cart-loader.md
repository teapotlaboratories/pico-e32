# 2026-07-17 — SD cart loader: fake-08 loads carts from the onboard microSD

Turns the fake-08 port from a one-cart demo (flash-embedded) into a **self-loading runtime**: it mounts the
board's onboard microSD and loads a `.p8`/`.p8.png` from it, falling back to the flash cart when there's no
card. Design grounded via a research workflow (fake-08 cart seam / IDF SD-SPI / board wiring), then every
load-bearing fact verified in code before writing a line.

## Why it wasn't actually parts-blocked

The 3.5" board has an **onboard microSD slot** on SPI (CS=1 / MOSI=2 / MISO=41 / CLK=42). The port doc
listed SD as "parts-blocked", but the slot is on-board — only a card (a commodity) is needed, not a parts
order. The SD pins are **disjoint from the i80 LCD** (WR35/DC36/CS37 + data `{47,21,14,13,12,11,10,9,3,8,
16,15,7,6,5,4}`), so the SD gets a **private SPI2 bus — no contention.** The "microSD bus shared with the
LCD" note in the board reference is a carry-over from the 4" board (DP-7); confirmed wrong for this board
against `board.cpp`.

## Design

- **Mount** — `firmware/pico-e32-fake08/main/sd_mount.{h,cpp}` (app): `esp_vfs_fat_sdspi_mount` on `SPI2_HOST`
  with the board's pins. **Graceful by contract:** any failure (no card, no pull-ups, bad FAT) logs, frees
  the bus, and returns — never aborts, so the caller falls back to flash. `format_if_mount_failed=false`
  (never auto-wipe a card).
- **Host scan** — the fork's `ESP32Host`: un-stub `listcarts()` (scan `_cartDirectory`, ported from
  `platform/gcw0`) + `getCartDirectory()`. The app sets `_cartDirectory = /sdcard` via the *shared*
  `setCartDirectory()`.
- **Cart ladder** — app `main.cpp`: if the SD mounted **and** holds a cart → `vm->LoadCart(carts[0], false)`
  (the `std::string` overload → file read over VFS); else the flash cart (test cart, or Celeste in the
  opt-in build).
- **`source/` byte-identical:** the file read (`get_file_contents`), `setCartDirectory`, and the
  absolute-path cart-load path (`cart.cpp`, `.p8` + `.p8.png`) all already exist in shared `source/`. Only
  additive app + fork code; `REQUIRES` gains `fatfs sdmmc esp_driver_sdspi esp_driver_spi`.

## Verified on hardware

- **Real 32 GB SDHC card mounts:** `SD mounted at /sdcard` (SDHC, 20 MHz).
- **Empty card → graceful fallback** to the flash cart — no crash.
- **Seeded a distinct `hello.p8` → loaded from SD and rendered** ("LOADED FROM SD!" green screen on the
  panel; log `loading SD cart: /sdcard/HELLO.P8`). Full round-trip: write → scan → load → parse → render.

## Two gotchas found + fixed

1. **FATFS readdir returns 8.3 short names UPPERCASE** (`HELLO.P8`), and fake-08's `isCartFile`
   (`filehelpers.cpp:121`) checks `.p8`/`.png` **case-sensitively** → carts were hidden (`isCart=0`). Fix:
   the fork's `listcarts` lowercases a copy of the name before the check; FAT open is itself
   case-insensitive, so the returned path keeps original case. Robust regardless of LFN.
2. **Long filenames need `CONFIG_FATFS_LFN_HEAP`** — and a build whose `sdkconfig` predates the change
   won't pick it up (IDF applies `sdkconfig.defaults` only when *generating* a fresh sdkconfig). After a
   regen, `readdir` returns long/lower-case names (`System Volume Information`, `logo.bmp`) as expected.

## Follow-ups

- **Cart selection** (choosing a cart, not just the first) needs input — parts-blocked. Today: first cart
  by readdir order, deterministic, logged.
- External line **pull-ups** are the #1 SD-SPI failure mode; this card worked, but a flaky card/socket may
  need them, or dropping `host.max_freq_khz` from 20 MHz (a commented one-liner in `sd_mount.cpp`).
