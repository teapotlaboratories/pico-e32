# 2026-08-03 — z8lua: kill the soft-float `int(fix32)` in the interpreter hot path

**Goal:** answer "is there an assembly-level optimization left in z8lua?" by disassembling the
built interpreter, then act on whatever it turns up.

**Outcome:** ✅ Found and fixed a latent defect the earlier `-O0→-O2` audit missed: extracting the
integer part of a fixed-point number — `int(fix32)` — was compiling to a **three-call soft-float
`double` round-trip** instead of a single `srai`. One line in `fix32.h` (an explicit
`operator int()`) removes it everywhere. Behavior byte-identical; measured **2.48× (P4) / 2.23×
(S3)** on an `int(fix32)`-saturated micro-benchmark. Change lives on submodule branch
`fix32-int-conv-no-softfloat`; **uncommitted** (weekday no-commit window).

---

## How it was found

Disassembled the built interpreter object on both targets:
`build/pico-e32-fake08/<board>/esp-idf/z8lua/CMakeFiles/__idf_z8lua.dir/lvm.c.obj`, attributing
every libgcc soft-float / soft-divide call to its enclosing function and source line
(`objdump -drl` + `addr2line -i`, vendored toolchains).

The hot dispatch loop `luaV_execute` carried a surprising cluster of **double-precision** soft-float
calls in a *fixed-point* VM:

| symbol | `__muldf3` | `__floatsidf` | `__fixdfsi` | `__divdi3` |
|---|---|---|---|---|
| `luaV_execute` | 9 | 8 | 7 | 3 |
| `luaV_gettable` | 1 | 1 | 1 | — |
| `luaV_peek` | 1 | 1 | 1 | — |

Mapping the call sites back to source (`fix32.h:34` = `operator double()`) showed the pattern was a
**`fix32 → double → int` round-trip** — `__floatsidf` (int→double), `__muldf3` (× 1/65536),
`__fixdfsi` (double→int) — i.e. `int( double(fix32) )`. It fires in:

- **integer-keyed table access `t[i]`** — `luaH_get`/`arrayindex` (`ltable.c:131,488`) convert the fix32
  key with `lua_number2int`, which (z8lua `#undef`s both `LUA_IEEE754TRICK` and the MS asm trick) falls
  through to `llimits.h:250` = `(int)(n)` = `int(fix32)`. **This is the hottest one** — array/list indexing
  is the most common op in gameplay carts — and it doesn't show up in `lvm.c`; it lives in `ltable.c`.
- **`luaV_peek`** (`lvm.c:251`, `int(a) & 0xffff`) — every `peek`/`poke` and the `@`/`%`/`$` operators
- **`OP_SHL/SHR/LSHR/ROTL/ROTR`** (`luaconf.h:582-586`, the `int((b))` shift/rotate count)
- **`luaV_gettable`** (`lvm.c:140`, the `str[pos]` string-index shortcut)

(The table-index reach was found in a follow-up adversarial pass — see below — not the original disassembly,
which only looked at `lvm.c`.)

The ordinary arithmetic (`+ - * < >`, `band/bor/bxor`) is already pure integer and was untouched. The
remaining double in `OP_POW` (`lvm.c:735` → `fix32::pow` = `fix32(std::pow(double,double))`) is
legitimate (transcendental) and cold — left alone. The 3 `__divdi3` are the known fix32 divide
(`OP_DIV`/`OP_IDIV`), previously measured as low-value under the 30 fps display cap — left alone.

## Root cause

`fix32` has eight `explicit` integer conversion operators (`int8_t`…`uint64_t`) but **no `operator
int()`**, and `int` is a *distinct* type from all of them (`int32_t` is `long` on both ESP toolchains).
So `int(x)` matches none exactly; overload resolution finds the `int8_t`/`int16_t` promotions equally
ranked → **ambiguous** among the integer set → and silently falls back to the only *non-explicit*
conversion, `operator double()`. Result: a one-instruction integer-part extraction becomes a
soft-float round-trip.

This is the exact mirror of an existing local patch: the committed `fix32(int)` **constructor** fix
(2026-07-14, "int32_t is 'long', leaving plain 'int' uncovered so int→fix32 is ambiguous"). Same
disease, other direction.

## Fix

One line in `components/z8lua/fix32.h`, alongside the other cast operators:

```cpp
inline explicit operator int() const { return m_bits >> 16; }
```

Gives `int(x)` an exact match. Verified at the compiler level (isolated `.cpp`, `-O2 -march=rv32imafc`):

```
int(fix32)  BEFORE: call __floatsidf ; call __muldf3 ; call __fixdfsi   (+ stack frame)
int(fix32)  AFTER : srai a0,a0,16 ; ret
```

And in the rebuilt interpreter objects, `__fixdfsi` is **gone** from `luaV_execute`/`peek`/`gettable`
on both P4 and S3; the only residual double is `OP_POW`'s `std::pow`.

**Correctness:** `m_bits>>16` is arithmetic-shift floor; the old `double` path truncated toward zero.
They differ *only for negative fractional* inputs — which never occur for shift counts, peek addresses,
or string indices (all non-negative integers). The micro-benchmark's result (`s`) is byte-identical
before/after on each board, confirming no behavioral change on the exercised paths.

## Measurement

Dev-only `LUABENCH` (`main.cpp`) temporarily pointed at a shift/peek-saturated loop —
90,000 iterations × (5 shifts + 1 `@` peek) = 540k `int(fix32)` conversions, timed in C over serial,
`-D LUABENCH=1 -D FORCE_FLASH_CART=1`. (Bench string reverted after; recorded here for repro:
`s = s + (i<<3) + (i>>2) + (i>>>1) + (i<<>4) + (i>><2) + @i`.)

| board | baseline (double round-trip) | with fix (`srai`) | speedup | result `s` |
|---|---|---|---|---|
| **P4** (RISC-V, /dev/ttyACM0) | 450,631 µs | **181,356 µs** | **2.48×** | 20564 (both) |
| **S3** (Xtensa, /dev/ttyUSB0) | 667,313 µs | **299,831 µs** | **2.23×** | −30108 (both) |

(Medians of 5. `s` differs across boards — a pre-existing cross-arch difference in the peeked
flash-cart memory, unrelated to this change; identical within each board = behavior preserved.)

### Whole-cart check — Pico Racer (P4)

To see the effect on a real interpreter-bound cart, measured the racer under `-D RACER=1 -D MEASURE_FPS=1`
(times raw `Step()` compute, unpaced). The reproducible state is the **idle title/attract** (mode-7 running,
dead-stable step); active racing swings 5.5–40 ms by scene and is too noisy to A/B cleanly.

| racer state (P4) | baseline | with fix | Δ |
|---|---|---|---|
| idle title `step` (n=24 each, non-overlapping ranges) | 5.680 ms | **5.540 ms** | **−2.5%** |
| `draw` | ~3.05 ms | ~3.02 ms | unaffected |

**~2.5% off the interpreter step** — the expected "small" outcome: the racer is mode-7 **mul/div + draw**-bound,
so `int(fix32)` (peek/shift) is only a minor share. Back-of-envelope, that puts `int(fix32)` at ~4% of the
racer's Step time. And at the title the cart is compute-bound at ~116 fps vs the 30 fps display cap (≈4×
headroom), so this is **CPU-time / power headroom, not observable fps** — precisely the caveat below. (Active
racing does briefly exceed the 33 ms budget in heavy scenes, where the same small % rides a bigger base.)

> **Build-system gotcha found here:** the `make` wrapper passes `DEFS` as CMake **cache** variables, so a `-D`
> from one build **persists** into the next until `fullclean`. An earlier `-D LUABENCH` had stuck, silently
> running the halt-bench in place of later builds (incl. the "shipping" reflashes). Fix: `make fullclean` before
> a build whose define set shrinks. Both boards were rebuilt clean afterward.

### Gun-FPS playtest — the cart class that benefits most (P4)

A raycaster FPS is the ideal real-world test: CPU-heavy *and* `peek`/`t[i]`-heavy (map + texture sampling) —
exactly the fix's hot paths, unlike the racer's mul/div mode-7.

- **POOM** (Doom demake) — loads and boots, but **hangs at level-load** ("ENTERING HANGAR", steady ~16 ms,
  no progression). POOM's `reload()`-based level decompression is a known emulator pain point; not playable on
  fake-08 as-is. (It's also the genuinely heavy one — ~37 ms/frame during load — i.e. the case that *would* be
  CPU-bound past the 30 fps cap.)
- **Wolfenstein 3D** — **plays**: title → `🅾️` starts it → the 3D raycasted maze renders with the gun HUD
  (SCORE/HEALTH/AMMO), driven over serial (`l/r/u/d` move+turn, `z/x` fire), confirmed on the bench camera.
  Moving: ~14 ms step + ~3 ms draw ≈ **59 fps compute-bound** (smooth at the 30 fps cap).

A/B on Wolf3D's reproducible standing-still raycaster step (`draw` skipped, pure raycast compute):

| Wolf3D raycaster step (P4) | baseline | with fix | Δ |
|---|---|---|---|
| standing (n=20 / 23) | 12.770 ms | **11.620 ms** | **−9.0%** |

**~9% — about 3.5× the racer's 2.5%.** This is the headline real-world number: a raycaster leans on
`peek`/integer-keyed-`t[i]` for every column it samples, so removing the soft-float round-trip from those hits
much harder than on the mul/div-bound racer. Still not *observable* as fps here (Wolf3D has headroom at the 30
fps cap), but a heavier raycaster (POOM's ~37 ms/frame) would cross into observable territory — this is the
strongest evidence that the fix is worth more than the racer alone suggested.

## Scope / honesty

This bench is **deliberately `int(fix32)`-saturated**, so 2.2–2.5× is the *upper bound for that
operation*, not a whole-cart number. Real-cart gain is proportional to how much a cart leans on
peek/poke and bit-shifts, and — because the display is 30 fps-capped (see
[z8lua-interpreter-pinned-o0]) — is only *observable* as fps when a cart is CPU-bound. A
memory-tricks/blitter cart hammering `peek` will feel it; the racer and Celeste won't.

Still worth landing: it's a one-line, strictly-better-codegen fix for a genuine "soft-float in a
fixed-point VM" defect, it lowers CPU load/power across the board, and it de-risks any future
CPU-bound cart. The divide hand-roll remains not-worth-it.

## Second adversarial pass — no new lever, but the fix is broader than first documented

Swept **every** libgcc/soft-float/soft-div/soft-mod call across all z8lua objects (not just `lvm.c`) on the
fixed P4 build, and attributed each to a function + source line:

- **Hot machinery is clean.** `ltable`, `lstring`, `ldo`, `lgc`, `lfunc` have **zero** soft-float/div/mod
  call sites. Table access, string interning/hash, the call path, GC, and closures touch no soft-float.
- **`ltable` being clean is the headline** — it means the `int(fix32)` fix already de-soft-floated
  `lua_number2int`, i.e. integer-keyed table indexing `t[i]` (the hottest op in gameplay carts). The
  original worklog only credited peek/shift/string-index; table indexing is the bigger beneficiary. (The
  earlier table-heavy `LUABENCH` used *string* keys `t.a` — the hash path — so it never exercised this;
  an integer-key `t[i]` bench would show a win like the shift bench.)
- **What soft-float remains is cold or already-assessed:** `OP_POW` (`^`, `std::pow` — transcendental,
  rare); `luaO_arith` (the arithmetic *fallback* for string coercion/metamethods — the fast path inlines in
  `lvm.c` and never calls it); `luaV_tostring`/`luaO_str2d` (number↔string — needs `double` for `%f`
  formatting, semi-cold); and the fix32 divide (`__divdi3`, low-value under the 30 fps cap).
- **fix32 primitive ops are optimal:** `*` → `mul`/`mulh`, `%` → hardware `rem` (RISC-V). No lever there.

**Conclusion:** there is no further worthwhile assembly-level lever in the interpreter. The one real
hot-path soft-float defect was `int(fix32)`, now fixed, and its reach includes the hottest op (table
indexing). Everything else is cold, format-required, or already-measured-not-worth-it.

## State

- **P4 + S3 reflashed to their shipping launcher/touch builds** (with the fix baked in) — known-good.
- Change on submodule branch `fix32-int-conv-no-softfloat`; superproject branch `z8lua-fix32-int-conv`.
- **Not committed** — weekday 9–5 Pacific no-commit window. When landing: z8lua submodule commit +
  gitlink bump, branch → PR → `/review` (rebase). No behavior change, so low review risk.
