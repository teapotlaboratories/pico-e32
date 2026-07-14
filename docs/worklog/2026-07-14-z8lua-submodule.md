# 2026-07-14 — z8lua: vendored copy → git submodule

**Goal:** turn `components/z8lua/` from a hand-vendored source copy into a proper git
submodule, adapted to build under ESP-IDF **without diverging from upstream more than
strictly necessary**.

**Outcome:** ✅ `components/z8lua` is a submodule tracking branch `pico-e32` of
`teapotlaboratories/z8lua` (a fork of `jtothebell/z8lua`, branch `z8lua-3ds-switch`) at
`d10747d`. The vendored tree is **byte-identical to upstream except one `fix32.h` patch**;
all ESP-IDF integration lives in the build files. Host build verified.

---

## Context / why a fork

z8lua is PICO-8's fixed-point Lua 5.2 dialect (`LUA_NUMBER = z8::fix32`, a C++ type). It
needs one genuine portability patch to build on the xtensa toolchain, so it can't be a
submodule pointing straight at upstream — the patch has to live in a fork we control.

- **Fork:** `teapotlaboratories/z8lua` (same org as the project).
- **Base:** `4cf7de4` on branch `z8lua-3ds-switch` — that repo's *actual* z8lua dialect line.
  (Its `master` branch tracks **vanilla Lua**; don't rebase onto it — noted in `UPSTREAM_SHA.txt`.)

## The one thing that took two tries — how to compile a C++ dialect shipped as `.c`

`fix32` is a C++ type, so the Lua core must compile **as C++**. The question was *how*.

- ❌ **First attempt (wrong):** renamed all 30 compiled `.c` files to `.cpp` and deleted the
  units we don't compile. This works but is a **bad way to vendor** — it rewrites the whole
  tree, makes every upstream rebase a rename war, and buries the divergence. (Feedback was
  blunt and correct: "nobody renames a `.c` to `.cpp` just to integrate.")
- ✅ **Correct approach:** leave the tree byte-identical to upstream and tell the *build* to
  use the C++ compiler. This is exactly what **upstream's own makefile does** —
  `CC = g++ -std=c++17` (the `g++` driver treats `.c` as C++). We mirror that:
  - **ESP-IDF** `CMakeLists.txt`: register the core `.c` files and
    `set_source_files_properties(… PROPERTIES LANGUAGE CXX)`.
  - **Host** `Makefile`: compile the same `.c` with `g++ -x c++`.
  - **Excluded units** (`liolib`/`loslib`/`loadlib`/`ltests` — host OS/dlopen deps `linit`
    never opens) are simply **left out of the source list**, not deleted.

## Fork branch — 2 clean commits atop `4cf7de4`

| commit | change |
|--------|--------|
| `7f91ff0` | `fix32.h`: implicit `int`/`unsigned int` ctor for toolchains where `int != int32_t` (xtensa). Mirrors upstream's `long`/`unsigned long` template; type-safe `enable_if` (no-op on x86); covers both signed+unsigned (upstream's `#ifdef __3DS__` block does `int` only). The **only** source edit. |
| `d10747d` | Add ESP-IDF component build: `CMakeLists.txt` (compiles the `.c` as C++ via `LANGUAGE CXX`, `LUA_USE_LONGJMP`, `-w`), `UPSTREAM_SHA.txt`, `LOCAL_PATCHES.md`. No renames, no deletions. |

Diff vs upstream: **3 files added, 1 modified, 0 renamed, 0 deleted.**

## Verification

- **Zero-divergence check:** `git diff --name-status 4cf7de4 HEAD` = `A CMakeLists.txt`,
  `A LOCAL_PATCHES.md`, `A UPSTREAM_SHA.txt`, `M fix32.h`. No `R`/`D` entries.
- **Compiles as C++ + runs:** luabench host benchmark built through its real Makefile
  (`g++ -x c++` over the 30 core `.c` files) against the submodule — clean build, benchmark
  ran (~305 ms total, 30 fps budget = 33 ms/frame; Gate `#2` is on-device).
- **Caveat:** the host is x86, so it can't exercise the xtensa `fix32` fix — that's the
  on-device Gate `#2` run.

## Also

- **Dropped the stray `components/z8lua/bench.h`** — a stale duplicate of the canonical
  `firmware/pico-e32-luabench/bench/bench.h`; both builds now resolve the canonical header.
- **Rule added** to `.ai/AGENTS.md` → *Porting / adapting upstream code*: **least-destructive
  vendor edits** — never rename an upstream file to change how it compiles (set the language in
  the build), never delete to exclude (leave it out of the source list); deviations recorded in
  the vendoring notes.

## Next steps

- Phase-0 hardware bring-up: Gate `#1` (ILI9488 ≥ 30 fps @ 256²) and Gate `#2` (z8lua
  throughput ≤ 33 ms/frame) on the real board.
