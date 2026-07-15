# 2026-07-14 — Phase-0 Gate #2: z8lua throughput on real hardware

**Goal:** first on-device bring-up. Two things at once: (1) prove the **new** z8lua
integration (compile `.c` as C++ via the build, no renames) actually builds for the
ESP32-S3 target under `idf.py` — only the host build was verified so far; (2) flash the
luabench harness and run **Gate #2** (interpreter throughput ≤ 33 ms/frame of work → 30 fps).

**Status:** in progress.

---

## Hardware / environment (confirmed, not assumed)

- **Board on `/dev/ttyUSB0`** — Silicon Labs CP210x UART bridge (USB `10c4:ea60`).
- **Chip:** ESP32-S3 (QFN56) rev v0.1, **2 MB embedded PSRAM (AP_3v3)**, **16 MB flash**,
  40 MHz crystal, MAC `48:27:e2:07:43:cc`. → matches the Makerfabs ILI9488 **N16R2** prototype
  (16 MB flash / 2 MB PSRAM); confirms the `board-psram-2mb-n16r2` note.
- **Toolchain:** vendored ESP-IDF v5.4.2, xtensa-esp-elf GNU 14.2.0 (`vendor/.espressif`).
- Serial device is `root:dialout` mode `crw-rw----`; `argonite` isn't in `dialout`, so opened
  it with `chmod 666 /dev/ttyUSB0` (throwaway box). Makefile default `PORT` is `/dev/ttyACM0`
  — the real port here is `/dev/ttyUSB0`, so builds/flashes pass `PORT=/dev/ttyUSB0`.

## Build for target (the real test of the new z8lua approach)

`make build APP=pico-e32-luabench BOARD=makerfabs-ili9488` — this is the first time the new
integration (upstream `.c` + `set_source_files_properties(… LANGUAGE CXX)`) is compiled by
the xtensa toolchain rather than host g++.

- CMake configure OK (target esp32s3, C/CXX = GNU 14.2.0).
- ✅ **Build clean, exit 0.** The proof the new approach works on the target toolchain:
  the log shows `Building CXX object …/z8lua/…/eris.c.obj` etc. — **30 z8lua `.c` files
  compiled as CXX objects, 0 as C objects, 0 errors.** So `set_source_files_properties(…
  LANGUAGE CXX)` correctly drives the xtensa g++ over the upstream `.c` — no renames, exactly
  the intent.
- App links: `pico_e32_luabench.bin` = `0x54cb0` (347 KB), 67% of the 1 MB app partition free.

## Gate #2 run

**First flash — hit a Task Watchdog, not a z8lua problem.** Booted fine and started the bench:
- `chip: ESP32-S3 rev1, 2 core(s)`
- `heap: internal free 378079 B, PSRAM free 2094956 B` (~369 KB SRAM, ~2.0 MB PSRAM — the 2 MB unit)
- Started the `[heap = internal SRAM]` case table, then **Task WDT triggered (~5.5 s): IDLE0 (CPU 0)
  did not reset in time; task `main` running.**

Root cause: the timed loop in `run_cases` (`for r in reps: lua_pcall`) runs flat-out in `app_main`
with no yield, so the idle task on CPU 0 starves. TWDT config was `INIT=y, TIMEOUT_S=5, PANIC=n` —
so it only warns + backtraces (no reset), but that print steals time from the timed case it fires
in, corrupting the measurement.

**Fix:** disable the Task WDT for this benchmark build (`CONFIG_ESP_TASK_WDT_INIT=n` in the app's
`sdkconfig.defaults`) — it's irrelevant to a throughput measurement. Rebuild + reflash + re-measure.

### Clean measurement (WDT off, 240 MHz, `-O2`)

`chip: ESP32-S3 rev1, 2 core(s); internal free 378475 B, PSRAM free 2094956 B`

| case | internal SRAM | PSRAM | M inner-iter/s |
|------|--------------:|------:|---------------:|
| fixed-point arithmetic | 19281.56 ms | 19281.56 ms | 0.104 |
| pico-8 sin/cos sweep | 3434.86 ms | 3471.55 ms | 0.058 |
| table churn (alloc+read) | **OOM (RUN ERROR)** | 7560.17 ms | 0.013 |
| **total** | 22716 ms (+OOM) | 30313 ms | |

**Config verified good** (so these are real, not a crippled build): CPU 240 MHz;
`COMPILER_OPTIMIZATION_PERF` (`-O2`); z8lua's `lvm.c` confirmed compiled `-O2` by
`xtensa-esp32s3-elf-g++` (via `compile_commands.json`); PSRAM quad @80 MHz.

### Interpretation vs the Gate #2 target

The benchmark's "iter" is **one inner Lua-loop iteration** (`s=s+i*0.5-i/3`), i.e. **~5–8 VM
instructions**, not one. So fixed-point `0.104 M iter/s` ≈ **~0.5–0.8 M Lua VM instructions/sec**.

- **PICO-8 target** (feasibility doc): ~**4 M VM inst/sec**. → we are **~5–8× slower, unoptimized.**
- Doc's re-plan threshold: *">3–4× slower after fix32 **+ bytecode** → 30 fps-only or ESP32-P4."*
  We're over it — **but this baseline has none of the optimizations the threshold assumes.**
- **Reconciling fact:** the doc cites PicoPico running **Celeste at ~9 ms/frame on a plain
  240 MHz ESP32**. The silicon clearly does far better than 0.5 M inst/s, so the gap is our
  unoptimized setup, not a hard ceiling.

**Why this baseline is slow (root causes, measured):**
- **VM runs from flash (XIP).** No `IRAM_ATTR` anywhere in z8lua; the `lvm.c` dispatch loop is
  large and thrashes a **16 KB instruction cache**, and flash is in **DIO** mode (2-bit) — slow
  cache refills. This is the single biggest lever.
- xtensa software `fix32` multiply/divide vs native x86 64-bit mul (host is ~105× faster overall).
  The fixed-point case is divide-heavy (`i/3` every iter) — near worst case for fix32.
- **Internal SRAM (369 KB) can't hold the table-churn heap → Lua heap must live in PSRAM** (slower,
  2 MB here; the doc already flags N16R8/8 MB as the production target).

### Verdict (provisional) & next step

Gate #2 is **at risk but not decided**: the unoptimized baseline is ~5–8× off target, yet
PicoPico proves the hardware can hit real carts. **Do not re-plan silicon on this number alone.**
Next: an optimization pass, then re-measure against the ~3× pass window —
(1) flash **QIO** + **32 KB** I-cache (config-only, some boot risk);
(2) **place the VM dispatch (`lvm.c` `luaV_execute` + hot `l*.c`) in IRAM** (biggest lever);
(3) bytecode precompile (plan §10). Paused here to get direction before burning more flash cycles.

### Optimization pass — result #1: memory config is NOT the bottleneck

Added flash **QIO** (4-bit) + **32 KB** I-cache (was DIO + 16 KB) → **zero change**: fixed-point
still 19281 ms / 0.104 M iter/s, identical. **The benchmark is compute-bound, not flash-fetch-bound.**
The hot dispatch loop already fits in the 16 KB cache, so faster flash + bigger cache don't help —
**which also predicts IRAM placement (another fetch-locality fix) won't help.** So the earlier "VM
runs from flash = biggest lever" hypothesis is **refuted by measurement.**

**Real cost points at fix32 divide.** 19281 ms / 2.0 M inner-iters = 9.6 µs/iter ≈ **~2300 cycles/iter**
at 240 MHz; ~5–6 VM instructions/iter → **~420 cycles per VM instruction** (normal Lua is ~10–50).
That points at the `i/3`: on the 32-bit LX7, `fix32` divide is a **64-bit software division**
(`__divdi3`, hundreds of cycles). This case is divide-heavy — a **worst case**, not typical cart code.

**Next experiment:** decompose with a no-divide arithmetic case to confirm divide dominance and get a
representative throughput number (current cases stress the slowest ops: divide, trig).

### Optimization pass — result #2: divide is NOT it; the slowdown is uniform

Added `arith, no divide` (`i*0.33` vs `i/3`, same shape) + `empty loop` + `integer add only`. On device
(2.0 M inner-iters each, cycles/iter @240 MHz):

| case | ms | cycles/iter | vs ref Lua (~10–20) |
|------|---:|------------:|:--------------------|
| empty loop (FORLOOP only) | 2096 | **~252** | ~15× |
| integer add only | 6232 | ~748 | ~20× |
| arith, no divide (2 mul) | 18339 | ~2201 | ~30× |
| fixed-point (2 mul + div) | 19281 | ~2314 | — |

Divide costs only ~113 cyc (19281−18339) — **not the bottleneck.** The **empty loop is already ~15×
slow**, so the penalty is *uniform across every VM op*, not op-specific.

### Optimization pass — result #3 (ROOT CAUSE): fix32 operators not inlined into the VM

Disassembled the ELF (`xtensa-esp32s3-elf-objdump`). `luaV_execute` (**9.3 KB** giant function, in
flash) **calls the fix32 operators out-of-line** on the hot path: `operator*` (`ml`), `operator/`,
`operator+` (`pl`), `operator-` (`mi`), `operator<=` (`le` ×4), even `operator double` (`cvd` ×5), plus
`__muldi3`/`__divdi3` for mul/div. The operators are `inline` in `fix32.h`, but GCC's **large-function
inline heuristic refuses to inline them into the already-huge `luaV_execute`** — so every arithmetic VM
instruction pays call overhead. That is the uniform ~15× tax (present even in the empty loop).

**This is fixable and is the real lever** (not IRAM). Fix candidates: (a) build flags lifting GCC's
large-function inline limits for z8lua (`-finline-limit`, `--param large-function-growth/-insns`,
`max-inline-insns-single`) — no source edit; (b) `always_inline` on the fix32 operators in `fix32.h`.
Testing (a) first (least-destructive). Potential ~5–10× → would flip Gate #2 toward a pass.

### How PicoPico does it (the proven-fast reference)

Checked DavidVentura's PicoPico (runs Celeste ~9 ms/frame on a 240 MHz ESP32) and its z8lua fork
(`DavidVentura/z8lua`, branch `pico8`) against our jtothebell base:

- **`float` vs `double` — the big one.** DavidVentura's `fix32` converts via **`float`**
  (`fix32(float)`, `operator float()`); ours (jtothebell) uses **`double`**. The ESP32-S3 FPU is
  **single-precision only** — so every `double` op is *software-emulated*. Confirmed our ELF links
  `__adddf3`/`__muldf3`/`__divdf3`/`__floatsidf`/`__truncdfsf2`, and `luaV_execute` calls
  `operator double()` 5×. Switching fix32 to `float` uses the hardware FPU. **Reference-validated
  compute win, independent of the inlining fix.**
- **They do NOT use `always_inline`** (count 0 in their fix32.h) → the inline-into-`luaV_execute`
  problem is our own lever, not something the reference relies on.
- **Bytecode precompile (`to_c.py`)** speeds *parsing* (112 → 18 ms for a big cart), **not execution**
  — so it won't move this Gate #2 compute number (our chunks are pre-compiled once already).
- Their Celeste 9 ms/frame **includes rendering on the second core**; misc wins were a fixed-point
  `sin` (735 → 135 ms), "fastcalls" (~9%), and palette pre-encoded RGB565 (16.21 → 13.94 ms/frame).

**Takeaway:** two independent levers for us — (1) inline the fix32 operators into the VM (in progress),
(2) switch fix32 `double` → `float` to hit the S3's hardware FPU (as PicoPico does). Bytecode precompile
is a *load-time*, not throughput, optimization.

### Optimization pass — result #4: build-flag inlining FAILED; path is source patches

Tried lifting GCC's inline limits via `--param large-function-insns/-growth`, `max-inline-insns-single`
on the z8lua component (SHELL:-wrapped to survive CMake `--param` de-dup). Build succeeded but
**`luaV_execute` is byte-identical (0x246c) with all 34 fix32-operator calls intact** — GCC still
declined to inline them. Reverted (it also made `lvm.c` compile very slowly). Conclusion: the inline
lever needs **`always_inline` on the fix32 operators in `fix32.h`** (a source patch to the vendored
interpreter), not build flags.

**Both remaining levers are `fix32.h` source patches (to our fork):**
1. `always_inline` the arithmetic operators → fold them into `luaV_execute` (kills the ~15× call tax).
2. `double` → `float` conversions → use the S3 hardware FPU instead of software `__*df3` (PicoPico's
   proven approach; slightly changes conversion precision, which PICO-8 tolerates).

Decision point (paused): patching the vendored `fix32.h` is a real change to the interpreter — get
direction before editing the fork. Recommend doing both, then re-measure.

### Optimization pass — result #5: `always_inline` ≈ nothing (~1–6%)

Force-inlined the fix32 operators (`[[gnu::always_inline]]`) → call sites in `luaV_execute` 34 → 15,
but throughput barely moved: empty loop 2096 → 1962 ms, fixed-point 19281 → 19106 ms (~1%). **So the
out-of-line operator calls were NOT the bottleneck — the inlining hypothesis is refuted.** Reverted.

### Optimization pass — result #6 (THE LEVER): `-fno-jump-tables` was the culprit

With operators inlined the empty loop was still ~235 cyc/instruction → the cost is **VM dispatch itself**.
Checked the flags: ESP-IDF compiles z8lua with **`-fno-jump-tables -fno-tree-switch-conversion`**, so
`luaV_execute`'s ~40-way opcode `switch` is a **linear compare-chain** — `objdump` showed **168
conditional branches**, 0 indirect jumps. Every bytecode dispatch walked ~20 compares. That is invisible
to cache/inlining/divide — exactly matching results #1–5.

**Fix (build-only, one line):** re-enable `-fjump-tables -ftree-switch-conversion` for the z8lua
component. `luaV_execute` → **64 branches + 1 `jx`** (a real jump table). Measured speedup:

| case | baseline | jump-table | speedup |
|------|---------:|-----------:|--------:|
| empty loop | 2096 ms | 1602 ms | 1.31× |
| integer add only | 6232 ms | 2661 ms | 2.34× |
| arith, no divide | 18339 ms | 5921 ms | **3.10×** |
| fixed-point (÷) | 19281 ms | 6922 ms | **2.79×** |
| pico-8 sin/cos | 3435 ms | 1343 ms | 2.56× |
| table churn (PSRAM) | 7581 ms | 5779 ms | 1.31× |

Fixed-point **0.104 → 0.289 M iter/s** (~5–6 VM inst/iter → **~1.6 M VM inst/s**). Against the ~4 M/s
PICO-8 target that's **~2.5× off — inside the doc's ~2–3× Gate #2 pass window**, from ~5–8× before.
**Gate #2 is now provisionally passable, from a single build flag — no source edit.** And the
`double→float` FPU lever (PicoPico's approach) is still untried on top.

### Optimization pass — result #7: `double→float` = no measurable gain (here)

Switched fix32's read path `operator double()`→`operator float()` (+ `pow`/`ldexp`), matching
DavidVentura's fork (the ESP32-S3 FPU is single-precision; `double` is software-emulated). Result on
device: **byte-identical to jump-table-only** (fixed-point 6922.43 ms both; sin/cos ~1343 both).

Why: **z8lua's math is fixed-point.** The arithmetic operators work on integer `m_bits`; even `sin`/`cos`
use fixed-point lookup tables (`trigtables.h`), not `sinf`/`cosf`. Nothing in the benchmark — or in
typical PICO-8 math — hits the `double`/`float` conversion path (the `operator double()` calls are in
cold opcodes). Also had to keep the `double` ctor (parse-time constant construction; `eris.c` uses a
`double` literal that becomes ambiguous otherwise), so it's a partial switch anyway.

**Conclusion:** the `float` switch is a **no-op for our workload** — recommend reverting it to stay
least-destructive (an unmeasurable source patch to the vendored interpreter isn't worth carrying). The
whole Gate #2 win is the **one-line `-fjump-tables` build flag** (~2.8–3.1×).

### Fork investigation — is PicoPico's z8lua a better base? (and *why* PicoPico switched to float)

Prompted by "isn't PicoPico's z8lua more battle-tested?" — investigated the two forks and PicoPico
itself. Conclusion: **keep our current base.** Evidence:

- **Our base is `jtothebell/z8lua`, and `jtothebell` is the author of `fake-08`** — our Phase-1 runtime.
  So we're on the z8lua of our chosen player, not a random fork. It's also **more current** (base commit
  2026-07-04) than **DavidVentura/z8lua** (last push 2023-09-30). Both are permissive (Lua MIT + fix32 WTFPL).
- **PicoPico can't be adopted as a base:** (1) **no license** (all-rights-reserved); (2) its ESP port
  drives **ST7789/ILI9340 (SPI)**, not our ILI9488 i80 / ST7701 RGB — we write the display driver either
  way; (3) it's coupled to David's custom board + a sprawling SDL/3DS/Android/ESP tree. It's a great
  **reference**, not a foundation. fake-08 (849★, maintained, licensed) remains the right Phase-1 base.

- **Why PicoPico switched `double`→`float` (the actual question):** commits `f907376` "use floats
  instead of doubles" + `8b69c0c` "enforce using floats only", **May 2022**, when PicoPico targeted the
  **Raspberry Pi Pico / RP2040** (Cortex-M0+, **no FPU**). There, both float and double are
  software-emulated and single-precision float is faster/smaller — that's the *initial* reason, not an
  ESP32-FPU optimization. His later ESP32 dev log (`LOG.md`) reaches the opposite of "float is fast":
  quoting Espressif's guide *"even though ESP32 has a single-precision FPU, floating point is always
  slower than integer; if possible use fixed point"*, he converted hot paths to **fixed-point** — synth
  2× faster, and **`sin`/`cos` fixed 135 ms vs floating 735 ms (~7×)**. This confirms our result: the
  float switch is RP2040 legacy, immaterial on our S3 (our hot paths are already fixed-point).
- **Bonus:** PicoPico's own ESP-IDF build does **not** set `-fjump-tables` — so the ~3× dispatch win we
  found is an optimization the reference itself leaves on the table.

Sources: `DavidVentura/z8lua` commit history (`pico8` branch, `fix32.h`), `DavidVentura/PicoPico`
`LOG.md`, Espressif ESP-IDF speed guide, `jtothebell/fake-08`.

### The real thing — Celeste benchmarked on the S3

Replaced the synthetic "representative frame" with **PicoPico's actual `celeste.p8`** run headless on
z8lua: cart Lua + its map/sprite-flags loaded (so objects spawn and collision runs), the PICO-8
draw/audio/input API stubbed (no-op draws, `rnd`/table-helpers shim, scripted active input). Reloads
each gameplay level 3–15 and averages active-input `_update`+`_draw`. Code:
`firmware/pico-e32-luabench/bench/celeste_bench.cpp` (+ `celeste_cart.h`, generated from `celeste.p8`).

**Result on the ESP32-S3 (our `-fjump-tables` build, graphics/audio stubbed):**
**≈ 15.8 ms/frame average** over levels 3–15 (level object counts L3=18…L15=18, matching host — deterministic).

Two input profiles (dash-heavy vs typical) came out **essentially identical — 15.75 vs 15.86 ms** — so the
per-frame cost is **object-count-driven, not input-driven**; 15.8 ms is a *robust* number, not an artifact of
my scripting. The real variance is **per-room: 5–40 ms** (dense 18-object rooms ≈ 33–40 ms; typical rooms ≈ 10–12 ms).

- **30 fps (33.3 ms): most rooms pass with headroom;** the densest rooms (~33–40 ms) sit right at / just over the budget.
- **60 fps (16.6 ms):** the *average* is under it and lighter rooms clear it, but dense rooms don't — frame time is uneven.
- **vs PicoPico's 9 ms/frame (LX6):** consistent — our 15.8 ms is an *average that includes the densest rooms*;
  the median/lighter rooms (~10–12 ms) match PicoPico. First-party confirmation real Celeste is a solid 30 fps on the S3.
- **Dual-core makes it better than these numbers suggest:** in the real design, drawing + audio run on **core 1**,
  so ~15.8 ms is the *core-0 Lua budget* alone (draw isn't stealing from it) — the frame rate is
  `max(core-0 Lua, core-1 draw+audio)`, which is how PicoPico gets Celeste to 9 ms.

**Caveats:** graphics/audio stubbed; input scripted (but shown input-insensitive). Host ran the same cart at
~0.55 ms/frame → device ≈ 28× slower, consistent with the earlier host↔device ratio.

### Cheap speedup levers — measured, both dead ends

Since `-fjump-tables` is already applied, tested the two low-effort levers against the Celeste number:
- **`-Os` vs `-O2`:** identical VM speed (`fixed-point` 6922 vs 6931 ms, `empty loop` 1602 both; Celeste ~5%
  *slower* under `-Os`). The "`-Os` beats `-O2` on flash code" claim does **not** hold for the z8lua VM loop.
- **GC pause 200/400/800:** 16.3 / 17.5 / 15.8 ms — non-monotonic, within noise. Object count is bounded → GC isn't the cost.

**Conclusion: no easy 20–30% left — z8lua is at its `-O2`+jump-table optimum for this workload.** Remaining
levers all carry real cost (globals→locals auto-transform ~14%; SRAM/heap locality for table access; type-check
specialization = mini-JIT). The practical 60 fps paths are **dual-core** (draw/audio on core 1) and, for heavy
carts, the **ESP32-P4**. Both experiments reverted; benchmark back to `-O2` + a single clean Celeste pass.
(Full lever ranking: [`../reference/z8lua-speedup-research.md`](../reference/z8lua-speedup-research.md).)
