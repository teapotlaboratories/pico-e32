# references/ — third-party sources of record

Upstream repositories kept **as pinned submodules, read-only**, so board facts can be checked
against the vendor's own code instead of memory. Nothing here is built or flashed by this project,
and nothing here is edited — if something needs changing, it belongs in our tree, not theirs.

Why this exists: a pin map written from memory (and *cited* to a file nobody opened) is how this
project lost an evening. `.ai/AGENTS.md` → *Research & citations* requires verifying every cited
`file:line` against the real tree — these submodules are that tree.

## `makerfabs-parallel-tft-lvgl-lgfx` — **the rev-1 source of record**

[radiosound-com/makerfabs-parallel-tft-lvgl-lgfx](https://github.com/radiosound-com/makerfabs-parallel-tft-lvgl-lgfx)
— pinned at **`6d4b014`** (2024-05-24). **68 KB.**

`main/LGFX_MakerFabs_Parallel_S3.hpp` is where this project's ILI9488 pin map and LovyanGFX config
come from: **WR 35, RS 36, CS 37, RD 48, 40 MHz, `dlen_16bit=true`** — the **first revision** of the
Makerfabs 3.5" parallel board, which is the unit we have
([board reference](../docs/reference/pico-e32-makerfabs-boards.md)).

It matters because **Makerfabs' own current repo documents a different board**: their newer N16R8
(octal PSRAM) revision moved the LCD to WR 18 / DC 17 / CS 46, because octal PSRAM consumes GPIO
35/36/37. Following that map leaves this panel blank while every diagnostic reports success — see
[worklog](../docs/worklog/2026-07-16-panel-rev1-pinmap.md).

> ⚠️ **Makerfabs edited `firmware/SD16_3.5/SD16_3.5.ino` in place across the revision.** This repo
> (2024) captured the old pins and credits that path; the file at that path *today* says WR=18.
> Same repo, same filename, different hardware. **Cite the revision, not just the vendor.**

### Not vendored: Makerfabs-ESP32-S3-Parallel-TFT-with-Touch

[The vendor repo](https://github.com/Makerfabs/Makerfabs-ESP32-S3-Parallel-TFT-with-Touch) was a
submodule and was **removed** — it is 323 MB (two bundled Arduino/LVGL trees), it is not a build
dependency, and it documents the **wrong revision** for this board. Its only remaining value is as a
counter-example, which a URL provides for free. The two files worth reading, pinned by SHA:

- [`IDF/matouch/main/boards/3-5-ili9488-ft6236/config.h`](https://github.com/Makerfabs/Makerfabs-ESP32-S3-Parallel-TFT-with-Touch/blob/7670a17/IDF/matouch/main/boards/3-5-ili9488-ft6236/config.h) @ `7670a17` — the **newer** board's map
- [`IDF/matouch/main/boards/3-5-ili9488-ft6236/board.c`](https://github.com/Makerfabs/Makerfabs-ESP32-S3-Parallel-TFT-with-Touch/blob/7670a17/IDF/matouch/main/boards/3-5-ili9488-ft6236/board.c) @ `7670a17` — RD-high handling, i80 bus setup
- `hardware/` in that repo has schematics, if a wiring question ever needs settling.
