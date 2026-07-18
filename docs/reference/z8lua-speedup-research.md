# z8lua interpreter speedup — research & lever ranking

**Question:** how to raise z8lua's steady-state execution throughput on the ESP32-S3 (Xtensa
LX7 @ 240 MHz), from our measured **~1.6 M VM inst/s** toward PICO-8's **~4 M inst/s** budget,
*given we already enable `-fjump-tables` (~3×)*.

**Method:** multi-source deep research (20 sources fetched, 86 claims extracted, 25 adversarially
verified — 22 confirmed, 3 killed), 2026-07-14. Findings below are tagged with confidence; the
speedups are *evidence-based estimates*, **not yet measured on our codebase** — see caveats.

Related: [`pico-e32-runtime-feasibility.md`](pico-e32-runtime-feasibility.md),
[`../worklog/2026-07-14-phase0-gate2-luabench.md`](../worklog/2026-07-14-phase0-gate2-luabench.md).

---

## Bottom line

The evidence points **away** from more dispatch tuning, IRAM placement, and Lua→C AOT, and toward
**reducing per-opcode work** (globals→locals, type-check elimination) and **cheaper fix32 arithmetic**.
**But the #1 action is to *profile a representative cart on the S3 first*** — our leverage numbers are
*inferred* from prior art on other chips/workloads, not measured on our hot path.

## Ranked levers

| # | lever | est. gain | effort | evidence & catch |
|---|-------|-----------|--------|------------------|
| 1 | **globals→locals** in cart Lua | **~2.4×** (71→29 ms/frame) | low if manual; **hard if automatic** | Measured by the PicoPico author — but on **RP2040/Cortex-M0+, not Xtensa**; carts must run *unmodified*, so it needs an automatic bytecode/AST transform (author found automated bytecode opt "harder than expected"). Global read = const fetch + upvalue-hashtable index + hash; local = one `LOADK`. |
| 2 | **fix32 divide → reciprocal-multiply LUT** | promising, **unquantified on Xtensa** | medium | Every non-unity divide compiles to software 64-bit `__divdi3` (LX7 has no 64-bit HW divide); replace with a reciprocal LUT (leading bits index a Q16 table) + Newton correction. Must preserve PICO-8's exact 16.16 rounding/overflow. *(The "26% fewer instructions" figure was refuted in verification — magnitude unknown.)* |
| 3 | **type-check / tag-write elimination** for known-fix32 operands | high (the *documented* dominant per-op overhead) | **high** | "In practically every Lua instruction the interpreter performs several type checks… if types are known these can be eliminated, also removing tag-write memory accesses." This is what LuaJIT/Ravi/Deegen specialize away — hard without a JIT; open whether a lightweight pass fits z8lua. |
| 4 | ~~compiler flags: `-Os` / LTO~~ | **0% — measured, no win** | low | **Tested `-Os` on the Celeste + microbenchmarks (2026-07-14): identical VM speed** (`fixed-point` 6922 vs 6931 ms, `empty loop` 1602 both; Celeste ~5% *slower*). The research's "`-Os` beats `-O2` on flash code" does **not** hold for the z8lua VM loop — exactly its author's caveat. LTO has no easy ESP-IDF toggle (needs manual `-flto` archive plumbing). **Deprioritised.** |
| 5 | **IRAM placement of `luaV_execute`** | low for us | low-med | Helps only if the hot loop isn't already cache-resident — and GCC's jump/switch **dispatch table stays in flash** (`.rodata`), undercutting it. Consistent with our own null result (QIO + 32 KB cache = 0 → we're not fetch-bound). Would only pay off if the dispatch table were also moved to IRAM/DRAM. |
| 6 | ~~GC tuning (pause/stepmul)~~ | **0% — measured, no win** | low | **Tested pause 200/400/800 on Celeste (2026-07-14): 16.3 / 17.5 / 15.8 ms — non-monotonic, within noise.** Celeste's object count is bounded, so GC pressure is low. **Deprioritised.** |

## Dead ends (verified — do not pursue)

- **Computed-goto / tail-threaded dispatch:** only **~1.12×** on desktop Lua — the register VM already
  does substantial work per dispatch. *Our ~3× was jump-tables vs GCC's pathological `-fno-jump-tables`
  if-else chain, not computed-goto vs a normal switch.* (Xtensa's weaker branch prediction may make it
  somewhat larger, but unquantified.)
- **Lua→C AOT via the Lua C API (lua2c):** **0.25–0.75× — slower**, because generated C still routes
  every op through `lua_gettable`/`lua_pushvalue` (full VM overhead + C-call overhead). Only *non*-C-API
  native codegen helps, and the prior art ("lua-but-worse") is "promising, but buggy." High effort/risk.
- **LuaJIT / Deegen:** no Xtensa backend (Deegen is x86-64/LLVM only, +28% over LuaJIT). Not a drop-in.
  Confirms the plain-interpreter path is the only viable one — which is fine (PicoPico shipped Celeste
  on the z8lua VM).

## Caveats (what's *not* proven)

- The strongest source-level result (globals→locals, ~2.4×) was on **RP2040, not Xtensa LX7** — direction
  should hold, magnitude unverified on target.
- Two fix32 sub-claims were **refuted** by adversarial verification: that fix32 *multiply* is *always* a
  64-bit `__muldi3` (so multiply-cost magnitude is uncertain vs the well-established divide cost), and the
  specific "26% fewer instructions" reciprocal-divide figure.
- The `-Os` > `-O2` evidence is a single reputable-but-anecdotal blog on a GIF-decode workload whose own
  author cautions it may not apply to VM-like loops — treat as "measure both," not a recommendation.
- **All estimates must be validated against our own ~1.6 M inst/s baseline** — they are ranked by inferred
  leverage, not benchmarked on this codebase.

## Open questions → next work

1. **Profile first:** what fraction of per-frame cycles is actually fix32-`__divdi3` vs dispatch vs
   type-checks on *real* carts? A profile lets levers be ranked by measured hot-path share. *(→ Gate #2
   follow-up task; see the TODO.)*
2. Can globals→locals be an automatic bytecode/AST transform without breaking PICO-8 semantics?
3. Does a reciprocal-LUT fix32 divide preserve PICO-8's exact 16.16 rounding, and what's its real LX7 cycle count?
4. Does `-Os` (and/or LTO) actually beat `-O2` for *this* hot loop; is moving the dispatch table to IRAM/DRAM worth it?
5. Is a lightweight type-specialization pass (removing redundant checks/tag-writes for known-fix32 operands) feasible without a full JIT?

## Sources

Dispatch / VM design: [Optimising Lua (mcours PDF)](https://www.mcours.net/cours/pdf/info/Cours_pdf_Optimising_Lua.pdf) ·
[Lua register-VM paper (jucs05)](https://www.lua.org/doc/jucs05.pdf) ·
[Deegen / LuaJIT Remake](https://sillycross.github.io/2022/11/22/2022-11-22/) ·
[VM dispatch (Inner Product)](https://www.inner-product.com/posts/understanding-vm-dispatch/) ·
[lua-l computed-goto thread](http://lua-users.org/lists/lua-l/2016-02/msg00009.html) ·
[thesephist: Lua VM](https://thesephist.com/posts/lua/)
ESP32/Xtensa: [ESP-IDF speed guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/performance/speed.html) ·
[memory types](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/memory-types.html) ·
[RAM usage](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/performance/ram-usage.html) ·
[LTO toggle (esp-iot-solution)](https://github.com/espressif/esp-iot-solution/blob/master/tools/cmake_utilities/docs/gcc.md) ·
[-Os vs -O2 (atomic14)](https://www.atomic14.com/2026/03/06/size-better-than-speed) ·
[esp32.com perf thread](https://esp32.com/viewtopic.php?t=21711)
fix32 / arithmetic: [z8lua fix32.h](https://github.com/samhocevar/z8lua/blob/zepto8/fix32.h) ·
[SEGGER reciprocal division](https://blog.segger.com/algorithms-for-division-part-3-using-multiplication/)
Prior art: [PicoPico](https://github.com/DavidVentura/PicoPico) ·
[DavidVentura perf blog](https://blog.davidv.dev/posts/pico8-console-part-2-performance/) ·
[DavidVentura compiler-runtime blog](https://blog.davidv.dev/posts/picopico-compiler-runtime/) ·
[lua2c](https://github.com/davidm/lua2c) ·
[yocto-8](https://github.com/yocto-8/yocto-8) · [yocto-8 extmem](https://github.com/yocto-8/yocto-8/blob/main/doc/extmem.md)
