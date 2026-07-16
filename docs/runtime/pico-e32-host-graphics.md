# ESP32Host — the PICO-8 graphics surface

**Area backlog** for growing the minimal `ESP32Host` draw API into the surface a *real* cart
needs. Reachable from the master index [`docs/pico-e32-todo.md`](../pico-e32-todo.md); the plan of
record is [`docs/pico-e32-development-plan.md`](../pico-e32-development-plan.md) (Phase 1 / Gate #4).

## Why this, why now

Phase 1 is the full `ESP32Host` (fake-08 port): **input, audio, SD carts** — all three blocked on
parts not yet bought (I²C expander, MAX98357A, microSD). The **graphics half is not blocked on
anything**, and it is the gap between what runs today and a playable cart:

- **Gate #3** proved the *pipeline* (z8lua + `components/ili9488` + a frame loop, 161.5 fps) using a
  hand-written cart that only needs `cls`/`pset`/`rectfill`/`circfill`/`line`.
- **Gate #2** proved the *interpreter* on **real Celeste** (~15.8 ms/frame) — but with the draw calls
  **stubbed to no-ops** (`host_main.cpp:103`: `spr`, `sspr`, `map`, `print`, `camera`, `pal`, …).

So Celeste's *logic* runs at frame rate and the *display path* is proven, but nothing has ever drawn
a Celeste pixel. `spr` + `map` + `print` are what close that — and they are the bulk of Gate #4.

## The verification story (this is why it's worth doing now)

`host_main.cpp` already compiles **host-standalone** (`-DHOST_MAIN`, no display) and prints per-frame
framebuffer checksums; Gate #3 used that to verify rendering **byte-identical host↔device**, and it
caught a Lua-stack bug under ASan before ever flashing.

Extending that to **dump frames as images** means the whole graphics surface can be verified *by
looking at it*, with **no bench camera involved** — which matters while
[`BC-1`](../hardware/pico-e32-bench-camera.md#open-items) (rig framing/focus) is blocking the visual
gates.

**What this does and does not cover:**

| | Covered by host image dump | Covered only by a camera capture |
|---|---|---|
| sprite decode, palette, text, map layout | ✅ | |
| the panel's byte order (`swap_color_bytes`), MADCTL orientation, actual glass | | ✅ ([`BC-2`](../hardware/pico-e32-bench-camera.md#open-items)) |

They are complementary, not redundant: the host dump proves *the framebuffer is right*, the capture
proves *the panel shows it*. The existing `fb_hash` host↔device compare is what ties the two together
— if the hashes match and the host image is correct, the device framebuffer is correct too.

## Scope

| # | Item | Notes | Verified by | Status |
|---|------|-------|-------------|--------|
| HG-1 | **`__gfx__` extraction** — sprite sheet out of the `.p8` | `gen_celeste_cart.py` now emits `CELESTE_GFX[16384]`, one palette index per byte (unpacked; see *Memory* below) | ✅ decoded sheet renders as Celeste's sprites — Madeline, balloons, spikes, chest, CELESTE logo, all 16 palette entries used | ✅ **done** |
| HG-2 | **Frame dump on the host build** | `P8_DUMP=dir ./host` writes `frame_NNN.raw`; [`tools/p8_png.py`](../../tools/p8_png.py) renders it via the PICO-8 palette (stdlib `zlib`, no Pillow). Its `PAL` **must stay identical** to `PAL888` in `host_main.cpp`. Driven by the new [`firmware/pico-e32-host/host/Makefile`](../../firmware/pico-e32-host/host/Makefile) (`make dump`) | ✅ dumped frame of the trivial cart matches Gate #3's draw calls exactly (blue field, navy + green bars, grey midline, 12 circfills in cols 8–15) | ✅ **done** |
| HG-3 | **`spr(n,x,y,[w,h],[flip_x,flip_y])`** | The core of every cart. Sprite `n` at `(n%16*8, n/16*8)` in the sheet | Celeste's player/objects appear at plausible positions | 📋 **next** |
| HG-4 | **`map(celx,cely,sx,sy,celw,celh,[layer])` + `fget`/`fset`** | ⚠️ **See the shared-memory finding below — `__map__` alone is only half of Celeste's map.** `layer` filters on sprite flags (`__gff__`) | A recognisable Celeste room renders — **and rooms 16–31 render, not just 0–15** | 📋 |
| HG-5 | **`print` + the PICO-8 font** | ✅ **unblocked — the font is CC-0 straight from Lexaloffle** (see *Font licensing* below). Take the glyph data from the source, don't inherit an emulator's copy | Text is legible in a dumped frame | 📋 |
| HG-6 | **`pal`/`palt`, `camera`, `clip`** | Currently no-ops; Celeste uses `camera` for screen shake and `pal` for flashing | Room scroll/shake behaves; transparency correct | 📋 |
| HG-7 | **`sspr`** | Stretched blit; lower priority | | 💤 |

**Suggested order:** HG-1 + HG-2 first (they are the *verification harness* — without them every later
item is unverifiable), then HG-3 → HG-4 gets a recognisable room on screen, then HG-6, then HG-5.

## Verified finding — the sprite sheet and the map share memory (HG-4 depends on this)

Rendering the decoded sheet (HG-1) shows the **top half as Celeste's sprites and the bottom half as
noise**. That noise is not a decode bug — it is **map data**. PICO-8 aliases sprite rows 64–127 onto
map rows 32–63 (`0x1000–0x1FFF`), and Celeste uses it: its map is **128×64 cells** (32 rooms of
16×16, an 8×4 grid), but the `__map__` section only carries **4096 B = rows 0–31**.

Decoding gfx rows 64–127 as map bytes (a gfx byte packs 2 px, **low nibble = left pixel**, so
`tile = px[2x] | (px[2x+1] << 4)`) yields exactly 4096 bytes whose statistics match the known-real
`__map__` section:

| | zeros (empty) | tile id < 64 | max id |
|---|---|---|---|
| gfx rows 64–127, decoded as map | 40% | 95% | 127 |
| `__map__` rows 0–31 (known real) | 36% | 96% | 104 |

**Consequence for HG-4:** a `map()` fed only from `__map__` renders **half the world** — rooms 0–15
of 32. The generator must also emit the shared region as map rows 32–63. Note `CELESTE_MAP` is
currently `[4096]` and non-`const` (poke-able) and is **already used by `pico-e32-luabench`** — check
that consumer before resizing it to 8192, so the Gate #2 benchmark doesn't break.

## Open questions

- ~~**HG-5 — where does the font come from?**~~ **Resolved — see *Font licensing* below.**
- **Cart assets stay out of the tree.** `assets/celeste.p8` and `celeste_cart.h` are third-party game
  code and **gitignored** — regenerate locally with `gen_celeste_cart.py`. Whatever HG-1 adds must keep
  it that way (generator committed, output not).
- **Memory (decided):** the sheet is emitted **unpacked, 16 KB** (1 px per byte) rather than 8 KB @ 4 bpp
  — it keeps the `spr()` inner loop free of a shift/mask per pixel, and 16 KB of flash is noise against
  the 78% free app partition. Revisit only if a cart pokes sprite memory expecting PICO-8's packed
  layout.

## Font licensing — take it from the source, not from an emulator (HG-5)

**Decision: use the PICO-8 font data directly; it is CC-0. Do not vendor another emulator's font file.**

Lexaloffle's own FAQ is unambiguous ([lexaloffle.com/pico-8.php?page=faq](https://www.lexaloffle.com/pico-8.php?page=faq),
asked about using the palette/font in your own projects):

> "Yes, please do. The palette and font are both available under a [CC-0](https://creativecommons.org/publicdomain/zero/1.0/) license."

Announced by zep at the time: [twitter.com/lexaloffle/status/873657107203080192](https://x.com/lexaloffle/status/873657107203080192)
— *"The #pico8 font is now available under a CC-0 license."* CC-0 is a public-domain dedication: no
attribution, no notice, no licence file, no copyleft. **This also retroactively covers the `PAL888`
palette already in `host_main.cpp`** — same sentence, same licence.

**Why not fake-08's copy** ([`source/fontdata.cpp`](https://github.com/jtothebell/fake-08/blob/master/source/fontdata.cpp)),
which was the obvious shortcut:

- fake-08 declares **MIT** ([`LICENSE.MD`](https://github.com/jtothebell/fake-08/blob/master/LICENSE.MD)),
  and that file carefully credits ~15 third-party components — **but says nothing about the font.**
- The only provenance in the file is a one-line comment: `//taken from tac08:
  https://github.com/0xcafed00d/tac08`. So the chain is PICO-8 → tac08 → fake-08 → us, and the file
  carries **no copyright notice at all**.
- That's awkward on its own terms: MIT's one obligation is preserving the copyright notice, and there
  isn't one to preserve. More basically, an emulator can license *its own code*; it cannot grant rights
  to data it took from PICO-8.
- **All of which is moot**, because the data is CC-0 at the source. Taking it from Lexaloffle's terms
  needs no chain, no notice, and no *Porting / adapting upstream code* code-map doc — we would not be
  porting anyone's code, just using public-domain data.

Note the distinction if the shortcut is ever revisited: the **glyph data** is CC-0 wherever you read it,
but a specific **file** (its encoding, layout, surrounding code) is that project's MIT-licensed work.
Transcribe the data; don't copy the file.

## Trademark — separate from, and stricter than, the font (flag for later)

The same FAQ draws a hard line between the CC-0 *assets* and the PICO-8 *name and logo*:

> "If you are making a separate software product, a run of multiple hardware units, PICO-8 related
> merchandise, or anything else -- please write to me first."

This project is a **DIY PICO-8 handheld**. As a personal one-off that is squarely fine, and nothing here
changes. But **"a run of multiple hardware units" is named explicitly** — so if pico-e32 ever becomes
more than one unit for personal use, or is distributed under the PICO-8 name, that is a
write-to-zep-first situation. Worth knowing now rather than at the point of ordering PCBs. (Naming this
because it is easy to conflate "the font is CC-0" with "PICO-8 branding is free" — the FAQ says the
opposite in the same breath.) Not legal advice; just what the vendor's own terms say.

## Not in scope here

Input, audio, and SD-card cart loading (the rest of Phase 1 / Gate #4) — those are parts-blocked and
tracked in the plan, not in this doc.
