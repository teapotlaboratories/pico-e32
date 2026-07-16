# 2026-07-15 — ESP32Host graphics: cart assets, the verification harness, and the font question

**Goal:** start the PICO-8 graphics surface ([`docs/runtime/pico-e32-host-graphics.md`](../runtime/pico-e32-host-graphics.md),
`HG-1`…`HG-7`) — the one Phase-1 item that is neither parts-blocked nor camera-blocked. Celeste's
logic already runs at frame rate (Gate #2) and the display path is proven (Gate #3), but `spr`/`map`/
`print` are **no-op stubs**, so no Celeste pixel has ever been drawn.

**Status:** 🔨 **in progress.** HG-1 ✅, HG-2 ✅, HG-5 ✅ unblocked; HG-3 (`spr`) next. Two findings below
change other people's work: one affects HG-4, one is a **caveat on Gate #2's headline number**.

> *(This log is being updated as the work happens, at each checkpoint — unlike
> [the bench-camera log](2026-07-15-bench-camera.md), which was reconstructed at the end.)*

---

## HG-1 ✅ — `__gfx__` extraction

`assets/gen_celeste_cart.py` already pulled `__lua__`, `__gff__` and `__map__` out of the cart, but
**never `__gfx__`** — which is precisely why `spr` was a stub: there were no sprites to draw.

The section is 128 lines × 128 hex chars, **one hex char per pixel**, left-to-right. Note this is *not*
PICO-8's in-memory packing (which puts 2 px per byte, low nibble = left pixel), so it decodes as a
straight char→value read — no nibble swap.

Now emitted as `CELESTE_GFX[16384]`, **unpacked** (1 palette index per byte) rather than 8 KB @ 4 bpp:
it keeps the `spr()` inner loop free of a shift/mask per pixel, and 16 KB of flash is noise against a
78%-free app partition. (Revisit only if a cart pokes sprite memory expecting the packed layout.)

**Verified by looking at it,** not by asserting a byte count: rendered the decoded sheet to PNG →
Madeline's sprites, balloons, snow, spikes, the chest, strawberries and the CELESTE title logo, all 16
palette entries in use. Round-tripped *out of the generated header*, so it tests the real artifact.

## HG-2 ✅ — the verification harness

[`tools/p8_png.py`](../../tools/p8_png.py): renders any indexed buffer (one palette index per byte) to
PNG through the PICO-8 palette. **Stdlib `zlib` only** — no Pillow, so it runs anywhere the repo does.
Its `PAL` table must stay identical to `PAL888` in `host_main.cpp`; that equality is the entire point,
since the tool exists to show what the firmware would draw.

`host_main.cpp` now dumps raw frames under `P8_DUMP=dir` (opt-in, so the existing checksum flow is
untouched). **Verified:** the dumped frame of the *existing* trivial cart is exactly what its Lua says
it draws — `cls(12)` blue field, `rectfill(0,0,127,10,1)` navy bar, `rectfill(0,118,127,127,3)` green
bar, `line(0,64,127,64,5)` grey midline, 12 animated `circfill`s in colours 8–15.

Why this comes first: it is the *camera-independent* way to see rendering, which matters while
[`BC-1`](../hardware/pico-e32-bench-camera.md#open-items) blocks the visual gates. Note what it adds
over `fb_hash`: **the hash only proves host and device agree — two identically-wrong framebuffers hash
just fine.** Only the image proves either is right.

### Side-finding: Gate #3's host build was never reproducible

Gate #3's worklog says `host_main.cpp` "compiles host-standalone (`-DHOST_MAIN`)" and that ASan caught a
Lua-stack bug there — but **no build command was ever committed or recorded**, and `pico-e32-host` had no
`host/` dir. The claim was true and the evidence unreproducible, which `.ai/AGENTS.md` → *Hardware &
flashing notes* ("capture the flash/monitor command you actually ran") exists to prevent.

Fixed by committing [`firmware/pico-e32-host/host/Makefile`](../../firmware/pico-e32-host/host/Makefile),
mirroring the established `pico-e32-luabench/host/Makefile` pattern (compile z8lua's upstream `.c` as C++
via `-x c++`, no renames; same `liolib/loslib/loadlib/ltests` exclude list as the component's CMake):
`make run` (hashes), `make dump` (frames + PNG), `make asan` (the pre-flash check that found the bug).

## Finding 1 — the sprite sheet and the map share memory (changes HG-4)

The rendered sheet's **bottom half looked like noise**. It is not a decode bug: PICO-8 aliases sprite
rows 64–127 onto **map rows 32–63** (`0x1000–0x1FFF`), and Celeste uses that region for map data.

Decoding gfx rows 64–127 as map bytes (`tile = px[2x] | (px[2x+1] << 4)`) yields exactly 4096 bytes
whose statistics match the known-real `__map__`:

| | zeros (empty) | tile id < 64 | max id | distinct |
|---|---|---|---|---|
| gfx rows 64–127, decoded as map | 40% | 95% | 127 | 92 |
| `__map__` rows 0–31 (known real) | 36% | 96% | 104 | — |

That is map data, conclusively. **Celeste's map is 128×64 cells** — 32 rooms of 16×16 in an 8×4 grid —
and `__map__` carries only **half** of it.

**Why it matters:** from the cart, `level_index() = room.x%8 + room.y*8` and `load_room` reads
`mget(room.x*16+tx, room.y*16+ty)` (`assets/celeste.p8:63`, `:1118`, `:1132`). So:

| levels | room.y | map rows | source |
|---|---|---|---|
| 0–7 | 0 | 0–15 | `__map__` |
| 8–15 | 1 | 16–31 | `__map__` |
| **16–23** | 2 | **32–47** | **shared gfx region** |
| **24–31** | 3 | **48–63** | **shared gfx region** |

A `map()`/`mget` fed only from `__map__` renders **rooms 0–15 and silently returns empty for 16–31** —
plausible-looking and wrong. HG-4 must feed it from both.

## Finding 2 — a caveat on Gate #2's headline number (not a bug)

`firmware/pico-e32-luabench/bench/celeste_bench.cpp:17` bounds-checks `mget` at **`y<32`**, returning 0
above that — the exact half-map behaviour above. **This does not invalidate the ~15.8 ms/frame result**:
the bench deliberately averages **levels 3–15 only** (`bench_pass()`, `for (int lv=3; lv<=15; lv++)`),
all of which live in rows 0–31, and the code says so (`:119` — *"all levels 0-15 are in `__map__`"*).
The measurement is honest and correctly scoped.

**But:** levels **16–30 were never benchmarked** — roughly *half the game*, and the later, generally
busier half — because their map data was not extracted. Gate #2's "solid 30 fps" conclusion therefore
rests on the first half. (Level 31 is the title screen — `is_title()` is `level_index()==31`.)

**Opportunity:** now that HG-1 decodes the shared region, the bench can cover all 31 gameplay levels.
Worth doing when HG-4 lands — if the later rooms are denser, the average moves, and that feeds directly
into the 30-vs-60 fps policy Gate #4 has to set. Filed as a follow-up rather than silently assuming the
number generalises.

## HG-5 ✅ unblocked — the font is CC-0 from the vendor

`print` needs the PICO-8 font, which lives in the console, not the cart. The obvious shortcut was
fake-08's [`source/fontdata.cpp`](https://github.com/jtothebell/fake-08/blob/master/source/fontdata.cpp).
Checked its licensing rather than assuming:

- fake-08 declares **MIT** ([`LICENSE.MD`](https://github.com/jtothebell/fake-08/blob/master/LICENSE.MD));
  GitHub reports `NOASSERTION` because it is an aggregate file. It credits ~15 components (z8lua,
  zepto8, tac08, PicoLove, SDL, LodePNG…) and **says nothing about the font**.
- `fontdata.cpp` carries **no copyright notice** — its whole provenance is `//taken from tac08`. So the
  chain is PICO-8 → tac08 → fake-08 → us, and MIT's one obligation (preserve the notice) has nothing to
  preserve. An emulator can license its own code; it cannot grant rights to data taken from PICO-8.

**All moot — the font is CC-0 at the source.** Lexaloffle's FAQ
([lexaloffle.com/pico-8.php?page=faq](https://www.lexaloffle.com/pico-8.php?page=faq)): *"Yes, please do.
The palette and font are both available under a CC-0 license."* (announced:
[twitter.com/lexaloffle/status/873657107203080192](https://x.com/lexaloffle/status/873657107203080192)).
No attribution, no notice, no licence file. **Same sentence retroactively covers the `PAL888` palette
already in `host_main.cpp`.**

**Decision:** take the glyph data from the source; do not vendor an emulator's font file. The *data* is
CC-0 wherever it is read, but a specific *file* is that project's MIT work — transcribe the data, don't
copy the file. This also avoids needing a *Porting / adapting upstream code* code-map doc: we are not
porting anyone's code.

**Trademark is separate and stricter** — the same FAQ: *"If you are making a separate software product,
**a run of multiple hardware units**, PICO-8 related merchandise, or anything else -- please write to me
first."* A personal DIY handheld is fine; more than one unit, or shipping under the PICO-8 name, is a
write-first situation. Easy to misread "the font is CC-0" as "the branding is free"; the FAQ says the
opposite in the same breath. Flagged in the area doc for whenever hardware decisions get real.

## What's verified / what isn't

**Verified:** `__gfx__` decode (by eye, from the generated header); the shared-region-is-map-data claim
(4096 B, statistics matching `__map__`); the level→map-row mapping (read from the cart source, cited);
Gate #2's scoping (read from the bench source, cited); font licensing (vendor's own FAQ).

**Not verified:** nothing renders yet — `spr` is still a stub. The 16 KB unpacked-sheet decision is
reasoned, not measured against a real `spr()` inner loop. Whether levels 16–30 change the Gate #2
average is **unknown**, not assumed.

## Next

- **HG-3** (`spr`) — the harness is ready, so it can be verified by dumping a frame and looking at it.
  Open choice: give `pico-e32-host` the Celeste sheet (it is **gitignored**, so guard the include with
  `__has_include` or the device build breaks for anyone without the asset), or test `spr` against a
  small synthetic sheet first and integrate Celeste with HG-4.
- **HG-4**: feed map rows 32–63 from the shared region; then extend the Gate #2 bench to levels 16–30.
