# 2026-07-14 — z8lua: vendored copy → git submodule

**Goal:** turn `components/z8lua/` from a hand-vendored source copy into a proper git
submodule, without losing the local patches that make it build for the ESP32-S3.

**Outcome:** ✅ `components/z8lua` is now a submodule tracking branch `pico-e32` of
`teapotlaboratories/z8lua` (a fork of `jtothebell/z8lua`), at commit `101a0c4`. Host
build verified. Parent-repo wiring goes out as a PR.

---

## Context / why

z8lua is PICO-8's fixed-point Lua 5.2 dialect (`LUA_NUMBER = z8::fix32`). It was
vendored as a flat source copy under `components/z8lua/` with two documented
divergences from upstream (see `LOCAL_PATCHES.md`):

1. **`.c` → `.cpp` rename** — `fix32` is a C++ type, so every compiled unit must be C++.
2. **`fix32.h` patch** — an implicit `int` / `unsigned int` constructor, needed because
   on xtensa-esp-elf `int32_t` is `long`, leaving plain `int` uncovered (ambiguous
   `int → fix32` under `-std=gnu++2b`). No-op on x86 where `int == int32_t`.

Because of those patches, z8lua **cannot** be a submodule pointing straight at upstream —
that would silently drop the renames + the fix32 fix and break the build. So the patches
have to live in a fork we control.

## Decisions

- **Fork location:** `teapotlaboratories/z8lua` (same org as this project), chosen over a
  personal fork to keep the project's dependencies together.
- **Patch strategy:** *fork + patches on a branch*, not a fresh snapshot repo. The fork
  keeps upstream's full history; our divergences are three real commits on branch
  `pico-e32` atop upstream `4cf7de4`. Re-pulling upstream becomes a clean rebase, which
  matches this project's divergence-tracking ethos.

## Findings (verified, not from memory)

- Diffed all compiled sources against upstream `4cf7de4`: **all 30 `.cpp` files are
  byte-identical to the upstream `.c`** → the rename is pure, no smuggled edits.
- **Only `fix32.h`** carries a content edit — exactly the documented hunk.
- **`bench.h` did not belong to z8lua.** It sat in `components/z8lua/bench.h` but is a
  benchmark-harness header, not upstream and not in `LOCAL_PATCHES.md`. It was a *stale
  duplicate* of the canonical `firmware/pico-e32-luabench/bench/bench.h` (the z8lua copy
  only declared `run_bench`; the real one also declares `run_cases`). The host Makefile's
  `-I` order (`-I$(Z8)` before `-I../bench`) made the host build accidentally pick the
  stale copy; it compiled only because `host_main.cpp` calls just `run_bench`. Dropping
  the stray copy makes both builds resolve to the canonical header — strictly an
  improvement, and required to keep the submodule clean.

## What was built

Fork branch `pico-e32` = upstream `4cf7de4` + 3 commits, authored as the project owner
(no AI attribution):

| commit | change |
|--------|--------|
| `37b905b` | Compile the Lua core as C++: rename 30 `.c` → `.cpp` (pure renames) |
| `6b6926f` | `fix32.h`: implicit `int`/`unsigned int` ctor for xtensa |
| `101a0c4` | ESP-IDF component wiring (`CMakeLists.txt`, `UPSTREAM_SHA.txt`, `LOCAL_PATCHES.md`); drop unused upstream units (`liolib`/`loslib`/`loadlib`/`ltests`, `makefile`, `README`, `*.dontcompile`) |

Parent repo: `git submodule add -b pico-e32` → `components/z8lua`, alongside the existing
`vendor/esp-idf` submodule.

## Verification

- **Tree parity:** fork branch tree ≡ the old vendored `components/z8lua/`, the only
  intended difference being the dropped stray `bench.h`. (Upstream's `.gitignore` is
  retained in the fork — build-invisible, since the ESP-IDF `CMakeLists` globs `*.cpp`.)
- **Compiles as C++ + runs:** built the luabench host benchmark through its real Makefile
  against the submodule — clean build, `bench.h` now resolves to `../bench/bench.h`,
  benchmark ran (fixed-point ~176 ms, sin/cos ~38 ms, table churn ~74 ms).
- **Caveat:** the host is x86, so it can't exercise the xtensa `fix32` int fix (as
  `LOCAL_PATCHES.md` notes). That belongs to the on-device Gate `#2` run.

## Next steps

- Open the parent-repo submodule-wiring PR (this change).
- Phase-0 hardware bring-up still pending: Gate `#1` (ILI9488 ≥ 30 fps @ 256²) and
  Gate `#2` (z8lua throughput ≤ 33 ms/frame) on the real board.
