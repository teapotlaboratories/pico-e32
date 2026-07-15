# Track B — z8lua throughput (Phase 0, Gate #2)

The project-defining benchmark: **is the ESP32-S3 (Xtensa LX7 @ 240 MHz) fast enough at
interpreting PICO-8's fixed-point Lua to hit the 30 fps floor?** This measures raw z8lua
throughput; it needs no display, so it's the cleanest way to answer the biggest risk.

## What it runs

Three microbenchmarks (`bench/bench.cpp`), each a small Lua chunk compiled once and
executed many times so we time pure interpreter work, not parsing:

| Case | What it stresses |
|------|------------------|
| fixed-point arithmetic | `fix32` multiply/divide/add in the VM loop |
| pico-8 `sin`/`cos` sweep | the PICO-8 math lib (fixed-point trig) |
| table churn | allocation + GC + table indexing |

On device it runs the whole set **twice — Lua heap in internal SRAM vs in PSRAM** — so you
can read the placement penalty directly (the plan keeps the hot Lua state in SRAM for this
reason).

## Two ways to build

### 1. Host (verification aid — already known to build & run)
```sh
cd host && make run
```
Builds z8lua + the benchmark natively and prints ms + M iter/s. The absolute numbers are
**not** comparable to the ESP32-S3 (a fast x86 core) — this only proves the runtime and the
harness are correct. Reference host run:

```
[default heap]
case                                ms      M iter/s
fixed-point arithmetic          ~171          ~11.7
pico-8 sin/cos sweep             ~37           ~5.4
table churn (alloc+read)         ~78           ~1.3
```

### 2. ESP32-S3 (the real Gate #2)
```sh
# from the repo root (ESP-IDF v5.1+):
make build flash monitor APP=pico-e32-luabench BOARD=makerfabs-ili9488
```
Reads the two-column SRAM-vs-PSRAM result over UART.

> **Compiles for esp32s3** with the vendored ESP-IDF v5.4.2 (`make build` → 347 KB image);
> the host build is additionally **run-verified**. What's left is reading the *numbers* on
> real silicon — the Gate #2 task itself.

## Reading Gate #2

**Pass:** the *internal-SRAM* total is comfortably inside a frame budget
(≤ 33 ms → 30 fps; ≤ 16.6 ms → 60 fps) for a representative per-frame Lua workload.
Reference lower bound: PicoPico runs Celeste at ~9 ms/frame on a *slower* ESP32 (LX6).
If even the SRAM result blows the budget after `-O2`, that's the trigger to consider the
ESP32-P4 (see `docs/design-specification/pico-e32-silicon-decision.md`).

## Notes on the runtime (verified from source)

- **z8lua compiles as C++**, not C: `LUA_NUMBER` is `z8::fix32` (a C++ type). Error
  handling is forced to setjmp/longjmp with `-DLUA_USE_LONGJMP` (so C++ exceptions can stay
  off). *This corrects the earlier plan note that said "compile as C."*
- **Numbers are 16.16 fixed point** (range ~±32768). A plain `for i=1,2000000` overflows the
  loop counter — real work is done with small bounds, repeated. Keep this in mind writing
  test carts.
- Vendored z8lua is `jtothebell/z8lua` — commit in `components/z8lua/UPSTREAM_SHA.txt`;
  upstream `.c` files renamed to `.cpp`, otherwise unmodified.
