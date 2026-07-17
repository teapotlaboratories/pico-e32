# ESP32Host — the PICO-8 graphics surface

> ⚠️ **Status (2026-07-17): this was a Phase-0 DE-RISKING HARNESS, now superseded.** The `HG-*` items
> below hand-wrote a from-scratch PICO-8 draw API (`spr`/`map`/`print`/`pal`/`camera`) to prove the display
> + z8lua render real Celeste content, camera-free. That job is **done** (HG-1…HG-6). The actual runtime is
> a **port of fake-08** ([`pico-e32-fake08-port.md`](pico-e32-fake08-port.md), and `.ai/AGENTS.md` →
> Primary goal + the 1-to-1 rule) — fake-08's own graphics **replace** this hand-written code, which stays
> as **reference/verification only** (the frame-dump harness and the findings below carry forward; the draw
> code does not). Don't extend the `HG-*` draw API further — port fake-08 instead.

**Area backlog** for the Phase-0 draw-API de-risking. Reachable from the master index
[`docs/pico-e32-todo.md`](../pico-e32-todo.md); the plan of record is
[`docs/pico-e32-development-plan.md`](../pico-e32-development-plan.md) (Phase 1 / Gate #4).

## Why this, why now

Phase 1 is the full `ESP32Host` (fake-08 port): **input, audio, SD carts** — all three blocked on
parts not yet bought (I²C expander, MAX98357A, microSD). The **graphics half is not blocked on
anything**, and it is the gap between what runs today and a playable cart:

- **Gate #3** proved the *pipeline* (z8lua + the board display + a frame loop, 161.5 fps) using a
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
| HG-3 | **`spr(n,x,y,[w,h],[flip_x,flip_y])`** | The core of every cart. Sprite `n` at `(n%16*8, n/16*8)` in the sheet; reads the active sheet (`g_gfx`, null-safe so the trivial cart is unaffected); default colour-0 transparency, with a minimal `palt()` to control it (the `pal()` **remap** is still HG-6) | ✅ **done** — `P8_SPRTEST=1 make dump` renders a 64-sprite grid from Celeste's real `CELESTE_GFX` (Madeline, balloons, chest, spikes, all recognisable), a 2×2 (`w=h=2`) block assembling a full Madeline, and an asymmetric "F" flipped four ways whose renders are **exact framebuffer mirrors** (checked numerically). Trivial-cart `fb_hash` **unchanged** (no Gate #3 regression). See [worklog](../worklog/2026-07-16-hg3-spr.md) | ✅ **done** |
| HG-4 | **`map(celx,cely,sx,sy,celw,celh,[layer])` + `mget`/`mset` + `fget`/`fset`** | The shared-memory finding is **resolved**: `gen_celeste_cart.py` now emits the **full 128×64 map** (rows 32–63 unpacked from gfx rows 64–127). `map()` draws each non-zero tile as a sprite (tile 0 skipped); `layer` filters on sprite flags via `fget`. Gate #2/luabench unaffected — its `mget` clamps to `y<32`, so the 4096→8192 resize is safe, and the first 4096 bytes are byte-identical | ✅ **done** — `P8_MAPTEST=1 P8_ROOM=n make dump` renders recognisable Celeste rooms; **rooms 16–31 (the shared region) render as real levels, not noise**; the `layer` filter is a verified strict subset (flag bits 0–2 used). Trivial-cart `fb_hash` unchanged. See [worklog](../worklog/2026-07-16-hg4-map.md) | ✅ **done** |
| HG-5 | **`print` + a 3×5 font** | ✅ done. Glyphs live in [`assets/pico8_font.h`](../../assets/pico8_font.h) (**authored**, not copied from any emulator's file — the most conservative licensing choice; see *Font licensing*). **📋 placeholder — backlog RT-FONT: rebuild it from a real PICO-8 font.** PICO-8 geometry: 3px glyphs, 4px advance, 6px line; lowercase → caps. `print(str,[x],[y],[col])` | ✅ **done** — `P8_TEXTTEST=1 make dump` renders the full glyph set and Celeste's real UI strings ("OLD SITE", "DEATHS:12", "MATT THORSON", "TIME 3:07", …) **all legible**. Trivial-cart `fb_hash` unchanged. See [worklog](../worklog/2026-07-16-hg5-print.md) | ✅ **done** |
| HG-6 | **`pal`, `camera`, `clip`** (`palt` done in HG-3) | All three applied at the single `px()` chokepoint, so every op honours them; identity defaults leave existing output byte-identical. `pal` is the DRAW palette (Celeste's flash) — the `p=1` screen palette isn't modelled (Celeste doesn't use it), nor does Celeste use `clip` | ✅ **done** — `P8_GFXTEST=1` verifies all three numerically (clip bounds a fill, `pal(8,12)` remaps, `camera` maps world→screen), and on a real room: `P8_CAMX=8` shifts it, `P8_FLASH=1` (`pal(i,7)`) flashes it all-white (death-flash). Prior tests byte-identical. See [worklog](../worklog/2026-07-16-hg6-pal-camera-clip.md) | ✅ **done** |
| HG-7 | **`sspr`** | Stretched blit. Done for completeness (owner call) even though the port supersedes it — Celeste barely uses it | ✅ **done** — `P8_SSPRTEST=1`: 1:1 `sspr` matches plain `spr` (identical), 2× is a correct nearest-neighbour upscale, flip-x mirrors, stretched region draws; all numeric. Trivial-cart `fb_hash` unchanged | ✅ **done** |

**Suggested order:** HG-1 + HG-2 first (they are the *verification harness* — without them every later
item is unverifiable), then HG-3 → HG-4 gets a recognisable room on screen, then HG-6, then HG-5.
**HG-1 through HG-7 are ALL done** — the entire PICO-8 draw surface is implemented and verified
(`spr`/`sspr`/`map`/`print`/`pal`/`camera`/`clip`, `palt`, `fget`/`fset`/`mget`/`mset`). This closes the
Phase-0 de-risking harness. Per the port plan, this hand-written code is now **reference/verification only**
— the next work is the **[fake-08 port](pico-e32-fake08-port.md)**, whose own graphics replace all of it.

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

**Consequence for HG-4 — RESOLVED (2026-07-16).** `gen_celeste_cart.py` now emits the full 8192-byte
map: rows 0–31 from `__map__`, rows 32–63 packed from gfx rows 64–127 (`tile = px[2x] | px[2x+1]<<4`).
Rooms 16–31 verified to render as real Celeste levels (not noise) via `P8_MAPTEST=1 P8_ROOM=16`. The
consumer check held: `pico-e32-luabench`'s `mget`/`mset` clamp to `y<32` (`celeste_bench.cpp:17,22`), so
they only index `[0,4096)` — the 4096→8192 resize is safe and the first 4096 bytes are byte-identical, so
**Gate #2 is unaffected**. (luabench's `y<32` clamp still hides rooms 16–31 from the *benchmark* — extending
it to `y<64` + running levels 16–30 would benchmark the full game; that's a Gate #2 re-scope, not HG-4.)

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

**What shipped (2026-07-16): the glyphs were AUTHORED** — 3×5 row-strings in
[`assets/pico8_font.h`](../../assets/pico8_font.h) (moved out of the firmware; `host_main.cpp` includes it
and decodes it) — not copied or transcribed from anyone's file. That is the most conservative option of
all — it carries no third-party data, CC-0 or otherwise, so none of the provenance analysis below can
bite. It renders Celeste's UI legibly (verified). The trade-off is fidelity: these are *PICO-8-style*
glyphs, not pixel-identical to PICO-8's.

> ### 📋 Backlog — RT-FONT: rebuild `assets/pico8_font.h` from a real PICO-8 font
> `pico8_font.h` is a **hand-authored placeholder**. Replace it with PICO-8's **actual** font, which is
> CC-0: export PICO-8's built-in font (as a PNG, or dump its font memory — 8 bytes/char, 8×8 bitfield,
> **bit LSB = leftmost pixel**) and convert it to the table — ideally via an `assets/gen_font.py` that
> emits `pico8_font.h`, the same pattern `gen_celeste_cart.py` uses for the sprite sheet. That gets
> pixel-exact, genuinely-CC-0 glyphs with zero hand-drawing, and drops the placeholder. **Blocked only on
> obtaining the font bytes** (needs a PICO-8 install to export; this dev box has none, and the exact bytes
> can't be fetched reliably). Format refs:
> [Lexaloffle BBS](https://www.lexaloffle.com/bbs/?tid=50948),
> [PICO-8 manual](https://www.lexaloffle.com/dl/docs/pico-8_manual.html);
> Lexaloffle also posts a vector [PICO-8.ttf](https://www.lexaloffle.com/bbs/?tid=3760) (for editors — the
> in-cart bitmap above is the faithful source).

**The original decision (still valid as a fidelity option): use the PICO-8 font data directly; it is CC-0.
Do not vendor another emulator's font file.**

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
