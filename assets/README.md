# assets — Celeste benchmark cart

The Gate #2 real-cart benchmark (`../firmware/pico-e32-luabench/bench/celeste_bench.cpp`) runs
**Celeste Classic** (`celeste.p8`, by Maddy Thorson & Noel Berry) headless on z8lua.

**These are not committed.** `celeste.p8` and the generated `celeste_cart.h` are third-party
game code — they are `.gitignore`d and must not be redistributed. Only this README and the
generator script are tracked.

## Setup (after cloning, before building)

```sh
python3 gen_celeste_cart.py
```

This fetches `celeste.p8` from [DavidVentura's PicoPico](https://github.com/DavidVentura/PicoPico)
(the same copy our ~15.8 ms/frame number is measured against) and writes `celeste_cart.h`:

| symbol | from | notes |
|---|---|---|
| `CELESTE_LUA` | `__lua__` | the game source |
| `CELESTE_MAP[4096]` | `__map__` | map **rows 0–31 only** — see below |
| `CELESTE_GFF[256]` | `__gff__` | sprite flags |
| `CELESTE_GFX[16384]` | `__gfx__` | 128×128 sprite sheet, **one palette index per byte** (unpacked); sprite `n` at `(n%16*8, n/16*8)` |

`celeste_bench.cpp` (Gate #2) includes it; the ESP32Host graphics work
([`docs/runtime/pico-e32-host-graphics.md`](../docs/runtime/pico-e32-host-graphics.md)) builds on it.

> **Half the map is not in `__map__`.** PICO-8 aliases sprite rows 64–127 onto map rows 32–63, and
> Celeste stores map data there — its map is 128×64 cells (32 rooms, 8×4). So `CELESTE_MAP` covers
> **levels 0–15 only**; levels 16–31 live in the shared region inside `CELESTE_GFX`. Anything reading
> the map must account for this or it will silently see empty rooms (`HG-4`).

If offline, drop a `celeste.p8` here manually and re-run the script.
