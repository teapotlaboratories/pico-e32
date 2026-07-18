# Building a PICO-8-Compatible Handheld on the ESP32-S3: A Technical Development Plan

## TL;DR
- **Feasible, but target 30fps first, not 60.** Running *actual* PICO-8 carts via a fake-08-style runtime (z8lua + fixed-point + a custom host layer) is proven possible on Xtensa ESP32-class silicon — DavidVentura's PicoPico already runs Celeste at ~9ms/frame on a 240MHz ESP32 — but the ESP32-S3's ~84 MB/s effective PSRAM bandwidth and the 2MB PICO-8 Lua heap are the two hard constraints that make locking heavy 60fps carts unrealistic.
- **Buy the right module: an ESP32-S3-WROOM-1-N16R8 (16MB flash / 8MB Octal PSRAM).** The 2MB-PSRAM ILI9488 board you have on hand is fine for display/Lua bring-up but has too little PSRAM to host the full 2MB Lua heap comfortably; your planned 4-inch board (ST7701, 480×480, N16R8) is the correct production target on the memory axis, though its RGB interface complicates scaling.
- **De-risk in this order, starting this week with hardware you own:** (1) benchmark z8lua-with-fix32 on the chip, (2) prove a 128×128→scaled framebuffer blits to the ILI9488 over the i80 parallel bus at ≥30fps, (3) run a trivial cart, then (4) tackle audio, input, and the 4-inch board. Lua-on-Xtensa speed and PSRAM framebuffer bandwidth are the two things most likely to kill the project — test them before you design a PCB.

> ## Update — 2026-07-14: Gate #2 measured on the ESP32-S3 (was untested; see caveat below)
>
> Ran `firmware/pico-e32-luabench` (z8lua + fix32, `jtothebell` base) on the Makerfabs **ILI9488 N16R2**
> (ESP32-S3, 2 MB PSRAM). Full log: [`worklog/2026-07-14-phase0-gate2-luabench.md`](../worklog/2026-07-14-phase0-gate2-luabench.md).
>
> - **Result: Gate #2 is passable.** Fixed-point throughput ≈ **0.29 M inner-iter/s ≈ ~1.6 M Lua VM
>   inst/s**, ~**2.5× under** the ~4 M/s PICO-8 target — inside the "~2–3×" go/no-go window. This is the
>   first S3 measurement (the "9 ms Celeste" figure was LX6/Wrover) and it confirms the LX7 assumption.
> - **The dominant cost was a build flag, not the interpreter or fix32.** ESP-IDF defaults to
>   **`-fno-jump-tables`**, which compiles `luaV_execute`'s ~40-way opcode `switch` as a linear
>   compare-chain (measured ~168 branches, ~20 compares per dispatch) — a uniform ~15× tax on *every*
>   VM op. Re-enabling **`-fjump-tables`** (one line in the z8lua component) gave **~2.8–3.1×** and is
>   what moves Gate #2 into the pass window. Ruled out (0–6% each): larger I-cache, QIO flash, IRAM
>   placement, `always_inline`, and the fix32 `double→float` switch. *(PicoPico itself does not set this
>   flag — it's an unexploited win.)*
> - **Refines "float math is the dominant cost" (below).** For z8lua that framing is misleading: all
>   game arithmetic and even `sin`/`cos` (`trigtables.h`) are **fixed-point integer** — the FPU/`double`
>   path is barely touched. PicoPico's 2022 `double→float` switch was an **RP2040 (no-FPU)** optimization;
>   on the S3, per Espressif's guide and David's own `LOG.md`, **fixed-point beats even hardware float**
>   (his `sin` fixed 135 ms vs float 735 ms). z8lua is already fixed-point, so `float` gave us **0 gain**.
> - **Base/fork decision:** keep **`jtothebell/z8lua`** (it *is* fake-08's z8lua, and is more current than
>   DavidVentura's 2023 fork). PicoPico stays a **reference only** — no license, and its ESP port targets
>   ST7789/ILI9340 SPI, not our i80/RGB panels.
> - **Still open:** internal-SRAM heap (~369 KB) can't hold an alloc-heavy workload (table-churn OOM'd) —
>   the Lua heap must live in PSRAM, reinforcing the **N16R8 / 8 MB** production-module call.
> - **Further speedup (if a cart needs it):** ranked levers in
>   [`z8lua-speedup-research.md`](z8lua-speedup-research.md) — but **profile a
>   real cart first**; classic dispatch tuning / IRAM / Lua→C AOT are low-yield here, and the candidates
>   (globals→locals, fix32-divide LUT, type-check elimination) should be ranked by *measured* hot-path share.

## Key Findings

**Prior art proves the concept but nobody has shipped a polished ESP32-S3 PICO-8 handheld.** The single most important reference is **PicoPico** (github.com/DavidVentura/PicoPico): it uses Sam Hocevar's **z8lua** (PICO-8's Lua dialect with a native 16.16 fixed-point number type), runs on an **ESP32 "Wrover" with 4MB PSRAM**, and reports verbatim: **"In ESP32: Celeste takes about 9ms / frame (rendering happens on the second core), including SFX."** That is comfortably inside a 16.6ms/60fps budget for that one game. Critically, PicoPico only works because David (a) precompiles Lua to bytecode at build time (cart parse 112ms → 18ms), (b) converted the zepto8 audio synth to fixed point ("I yoinked zepto8's synth and converted it to fix32; an example SFX went from ~25ms to ~2ms on the ESP32"), and (c) moved the Lua heap into PSRAM after abandoning an RP2040 port for lack of RAM. A community fork (lilka-dev/PicoPico) targets an actual ESP32-S3 handheld.

**fake-08 is the most complete open runtime and the best porting base, but has no ESP32 port yet.** fake-08 (jtothebell/fake-08, MIT license) is a clean-room C++ reimplementation of the PICO-8 VM (128×128, 16 colors, 4-channel audio at 22050 Hz, Lua via z8lua). It runs on 3DS, Switch, PS Vita, Wii U, Miyoo Mini/Bittboy, and even the Nintendo DS. Its clean host-abstraction layer (`SDL2Host.cpp`, `3dsHost.cpp`, etc.) is exactly what you would replace with an ESP-IDF host layer. It already runs on a 268MHz ARM11 (original 3DS), which is a useful proxy for what a 240MHz Xtensa core can manage.

**The 2MB Lua heap is the defining constraint.** Per the PICO-8 Wiki (Memory): "The memory used by Lua (global variables, local variables, tables, etc.) is entirely separate from the PICO-8 memory discussed above and is limited to 2 MiB" — checkable at runtime via `stat(0)`. This is why the RP2040 (264KB SRAM) is a dead end — both PicoPico and yocto-8 abandoned RP2040 ports over it. The ESP32-S3 solves this *only* with external PSRAM, and specifically you want **8MB Octal PSRAM**, not the 2MB on your current ILI9488 board.

**Display bandwidth, not compute, is the likely bottleneck on the 4-inch board.** Pushing 480×480 RGB565 (~460KB/frame) at 60fps requires ~27 MB/s just for pixel output, and the frame has to be composed in PSRAM whose effective bandwidth when the CPU is contending is limited. An ESP32 forum measurement ("Settings for best PSRAM Speed with ESP32 S3") found: "the ESP32 S3 PSRAM should be capable of reading at 80 MHz * 2 (ddr) * 1B (octal SPI) = 160 MB/s. However, in my tests I get ~84MB/s max when exceeding the cache size." The 3.5" ILI9488 (320×480, 16-bit i80 parallel) is actually easier to drive fast at small transfer sizes.

## Details

### 1. Prior-Art Survey

| Project | Language / VM | Target HW | Status / relevance to ESP32-S3 | Source |
|---|---|---|---|---|
| **fake-08** | C++ + z8lua | 3DS, Switch, Vita, Wii U, Miyoo, DS | Most complete open runtime; MIT; clean host layer; **no ESP32 port** — best base to port | github.com/jtothebell/fake-08 |
| **PicoPico** | C + z8lua (fix32) | ESP32 Wrover 4MB PSRAM; RP2040 (abandoned); 3DS; Android | **Directly proves ESP32 feasibility**; Celeste ~9ms/frame; bytecode precompile; fix32 synth | github.com/DavidVentura/PicoPico |
| **PeX Labs** | C++ (tac08 fork) | ESP32, then custom ESP32-S2 board, FreeRTOS | Rewrote tac08 HAL for FreeRTOS/ESP32; ran a simple controllable-sprite cart; no published FPS | medium.com/@getpexlabs |
| **yocto-8** | C++20 + modded Lua | RP2040 (abandoned), RP2350 planned | Documented the 2MB-heap problem in depth; RP2040 PSRAM-via-hardfault hack "too slow" | github.com/yocto-8/yocto-8 |
| **zepto8** | C++ + z8lua | Desktop | "Probably the best" emulator; source of fake-08's audio/PNG code; not embedded-targeted | github.com/samhocevar/zepto8 |
| **tac08** | C++ | Desktop | Runtime-only emulator; fake-08's sprite/cart code derived from it; PeX Labs' base | github.com/0xcafed00d/tac08 |
| **TIC-80 / retro8 / picolove** | C / Lua / LÖVE | Various | Reimplementations; retro8 is a libretro core; picolove needs LÖVE (not embeddable on MCU) | — |
| **MiSTer PICO-8** | zepto8 on ARM + FPGA | MiSTer FPGA | Hybrid ARM+FPGA; irrelevant to MCU but shows scaling approach (2×H, 1.75×V Bresenham) | github.com/MiSTerOrganize/MiSTer_PICO-8 |

**Related ESP32 emulator headroom.** retro-go (ducalex/retro-go) runs NES/GB/GBC/SMS/Genesis on the original ESP32 (Odroid-Go). Its documented approach is instructive: a 40MHz SPI display gives a **28fps full-frame fill rate**, so it uses **partial screen updates** (only changed regions) to reach an effective 60fps most of the time. This tells you that on plain SPI, full-frame refresh is the wall — the ESP32-S3's LCD_CAM parallel peripheral is what buys you more.

### 2. Feasibility, Memory & CPU Budget

**PICO-8's resource envelope.** 128×128 display; 16-color fixed palette; 4-bit-per-pixel framebuffer = **8KB**; 32KB cart ROM; **up to 2MB Lua memory** (`stat(0)`); 8192-token code limit; 60fps (or 30fps) target; 4-channel synth (SFX + music patterns). Per the PICO-8 Wiki (CPU): "There are 8,388,608 cycles per second (2^23), which is about 139,810 cycles per frame at 60 FPS, or 279,620 cycles per frame at 30 FPS." Since each Lua VM instruction costs ~2 cycles, that is roughly **4 million Lua VM instructions/sec** — a deliberately modest target. PICO-8's own stated minimum spec (per lexaloffle.com/pico-8.php) is "a Raspberry Pi (pictured) with ~700MHz CPU."

**ESP32-S3 specs.** Dual-core Xtensa LX7 @ 240MHz; 512KB internal SRAM; 384KB ROM; **single-precision FPU** (this matters — see below); LCD_CAM peripheral supporting 8-bit/16-bit Intel-8080 parallel and RGB; I2S audio; **no internal DAC** (unlike the original ESP32 — confirmed, you must use an external I2S DAC/amp or PDM). PSRAM options: Quad (up to 4MB typical) or **Octal (8MB)**; the recommended production module is **ESP32-S3-WROOM-1-N16R8** (16MB flash, 8MB Octal PSRAM). Note that Octal PSRAM permanently occupies GPIO 33–37.

**PSRAM bandwidth is the key hardware constraint.** Theoretical Octal PSRAM @80MHz DDR is ~160 MB/s, but the real-world ESP32-forum measurement above found **~84 MB/s max when exceeding the cache size**, because DMA and CPU contend for the same external-RAM path and the shared data cache is only effective for working sets under ~32KB. ESP-IDF exposes a 120MHz PSRAM option that improves this. The practical implication: the hot Lua VM state, the 8KB PICO-8 framebuffer, and audio buffers must live in **internal SRAM**; only the large, less-latency-sensitive Lua heap and cart/sprite/map data belong in PSRAM.

**Is a Lua interpreter at 30–60fps feasible? Yes for many carts, no for the heaviest.** The proof points:
- PicoPico runs **Celeste at ~9ms/frame on a 240MHz ESP32 (LX6)** — the S3 (LX7, same clock) should match or beat this.
- fake-08 runs on a 268MHz ARM11; the S3 is in the same performance class.
- The dominant costs are **float math** and **cart-parse time**, both of which are solved: z8lua's `LUA_NUMBER` is `z8::fix32` (16.16 fixed point), so all game arithmetic avoids the FPU entirely, and bytecode precompilation removes parse cost. David's microbenchmark showed fixed-point `sin` cut a 32,000-call workload from ~735ms to ~135ms in Lua. *(Measured on the S3 — see the 2026-07-14 update above: because z8lua is already fixed-point, "float math" is not the cost; the real per-instruction cost was the interpreter's opcode **dispatch**, fixed with `-fjump-tables`. The `double→float` switch bought nothing here.)*
- **LuaJIT is not an option** (no Xtensa backend). You are running the standard Lua 5.x interpreter (z8lua is based on Lua 5.2). Embedded Lua on ESP32 is well-trodden (Lua-RTOS ships Lua 5.3.4; Espressif ships a Lua ESP-IDF component).

**Verdict on framerate.** Simple-to-moderate carts (puzzle games, platformers like Celeste, most jam games) are realistic at **30fps, and many at 60fps**. Carts that assume a full 60fps `_update60` with heavy per-frame Lua object churn, large `tline`/`fillp` fills, or that push the 2MB heap will slow down or fail. This mirrors fake-08's own documentation: it warns that "performance is not great on Old 3ds systems" and "some games may experience slowdowns."

**Which PSRAM is enough?** The 2MB PSRAM on your current ILI9488 board can technically host a small Lua heap, but leaves almost no margin once you place cart data, decompression scratch, and any RGB565 scaling buffer there. **Target the N16R8 (8MB Octal).** This is exactly what both Makerfabs 4-inch and 4.3-inch boards use.

### 3. Display & Scaling Engineering

**The ILI9488 board (on hand).** 3.5", 320×480, ILI9488 controller, **16-bit parallel (i80/8080) interface, pixel clock up to ~20MHz** per Makerfabs. The ILI9488's notorious 3-byte-per-pixel (18-bit) SPI quirk is largely avoided in 16-bit parallel mode. Backlight is PWM-controllable on GPIO45. Using ESP-IDF `esp_lcd` with the LCD_CAM i80 driver and DMA, this panel is a good bring-up target: at 320×480×16bpp (~300KB/frame) and a ~20MHz pixel clock you are bandwidth-limited to roughly the 20–30fps range for full frames, which is fine for 128×128 content integer-scaled with letterboxing.

**The 4-inch board (production target).** Confirmed as **ST7701 controller, 480×480 IPS, driven over a 3-wire-SPI + RGB565 (parallel RGB) interface, Makerfabs rates it FPS>50**, on an ESP32-S3-WROOM-1-N16R8, with onboard speaker, LiPo charger, and SD. The RGB interface means the ESP32-S3 must continuously stream a full framebuffer from PSRAM via the RGB peripheral (with bounce buffers), which consumes PSRAM bandwidth *continuously*, not just at flip time — a real consideration given the ~84 MB/s ceiling. The ESP-IDF RGB-panel driver supports a bounce-buffer mode (DMA reads a small internal bounce buffer that an ISR refills from the PSRAM framebuffer) specifically to raise achievable pixel clock; the tradeoff, per the Espressif ESP-IDF forum (esp32.com/viewtopic.php?t=26793), is bandwidth contention: "if both the CPU and GDMA request access to the PSRAM, the bandwidth is divided 50-50 between the CPU cache and GDMA... as soon as the CPU also wants to do something with the image... the bandwidth will be reduced to 50%... If the LCD needs more than that 50%, you'll get corruption."

**Scaling 128×128 up.** Use **integer scaling with pillarboxing/letterboxing** to preserve pixel-perfect output:
- On the 320×480 ILI9488: 2× = 256×256 (fits with a border) or 3× = 384×384 (fits in the 480 dimension, borders on the other).
- On the 480×480 ST7701: 3× = 384×384 centered, with a 48px border all around — clean and pixel-perfect. (Non-integer 3.75× would fill the screen but blur pixels; avoid it for the retro aesthetic.)
- **Do the palette lookup + scale during the blit, not as a separate pass.** The efficient pattern (used by PicoPico) is to keep the native 4bpp/indexed 128×128 framebuffer in SRAM, pre-encode the 16-palette as RGB565 `uint16_t`, and expand+scale a line at a time into a small RGB565 line buffer that DMA pushes out. This avoids ever allocating a full-size RGB565 framebuffer in PSRAM on the i80 path.
- On the RGB (ST7701) path you cannot avoid a full-size framebuffer (the peripheral streams it continuously), so the scale must be written into that PSRAM framebuffer — another reason the i80 ILI9488 path is gentler.

**Should the final handheld use a smaller native display instead?** Strongly worth considering. A **240×240 SPI ST7789** (1.8× scale) or even a **native-ish 128×128 / 128×160 ST7735** (as PicoPico used) drastically cuts bandwidth and backlight power. The tradeoff: a 3.5"/4" screen looks and feels far better as a handheld, but a 1.9"–2.4" ST7789 at 240×240 is the sweet spot for battery life, pixel-fidelity (near-integer scale), and simplicity. **Recommendation: prototype on the 3.5"/4" boards you have, but seriously evaluate a 2.0–2.4" 240×240 ST7789 for the final build.**

### 4. Input, Audio, Power

**Buttons & the pin-budget problem.** PICO-8 needs 6 buttons (D-pad ×4, O, X) plus a menu/pause button. The catch: a **16-bit parallel TFT consumes ~16 data pins + control pins**, and Octal PSRAM eats GPIO 33–37, so free GPIO on the Makerfabs parallel boards is scarce. Two clean solutions:
- **Analog resistor-ladder** on a single ADC pin: wire all buttons through a resistor divider so each combination yields a distinct voltage. Costs one ADC GPIO. Simple, cheap, but simultaneous-press handling needs care (diode-OR or careful ladder design).
- **I²C GPIO expander** (PCF8574, 8 I/O, or MCP23017, 16 I/O) on the Mabee/Grove I²C bus that both Makerfabs boards expose. Costs zero extra GPIO beyond the shared I²C. This is the recommended approach — it keeps buttons off the crowded parallel bus and gives you interrupt-on-change.

**Audio.** The ESP32-S3 has **no internal DAC**, so you need either (a) an **I²S external DAC/amp** — the **MAX98357A** (Class-D, I²S in, built-in DAC) is the standard, cheap ESP32 choice; per Analog Devices' MAX98357A datasheet it "is capable of delivering 3.2W into a 4Ω load. The device outputs can be connected directly to a speaker load for filterless applications" (also 1.8W @ 8Ω; pin-selectable gains 3/6/9/12/15dB; I²S sample rates 8kHz–96kHz; no MCLK required, 3 pins: BCLK/LRCLK/DIN); the **PCM5102A** is the higher-fidelity line-out option for a headphone jack — or (b) **PDM output** via I²S into a PDM amp (MAX98358/SSM2537) or an RC low-pass filter. Note the 4-inch Makerfabs board already ships with a **3Ω 4W speaker and an onboard amp/speaker connector**, and its wiki notes some pins are assigned to I²S. Real-time synthesis of PICO-8's 4 channels is cheap once converted to fixed point (PicoPico: 2ms/SFX); **run audio synthesis + display DMA on core 1, Lua + game logic on core 0.**

**Power & battery.** The **backlight dominates** current draw on a large TFT — the ESP32-S3 itself runs ~40mA in normal mode without WiFi (per Adafruit's S3 measurements), while a 3.5"/4" backlight at full brightness commonly pulls 60–150mA. Budget accordingly:
- **Charging/PMIC:** TP4056 (cheap, linear, 1A, needs separate protection) or MCP73831 (compact) for simple builds; a proper PMIC (e.g., BQ-series) if you want fuel-gauging. The Makerfabs 4-inch board already integrates a LiPo charger. Add a fuel gauge (MAX17048) for a battery %.
- **Runtime estimates** (assume ~200mA total average: S3 both cores + moderate backlight + audio): a **1000mAh** LiPo ≈ 4–5h; **2000mAh** ≈ 8–10h; **3000mAh** ≈ 12–15h. At full brightness on the 4-inch panel, expect the lower end; dimming the backlight is the single biggest lever. USB-C for charge + data (the Makerfabs boards use dual USB-C, one native-USB one UART-bridge). Use light-sleep between frames only if you drop below 60fps headroom (marginal savings while gaming; big savings in menus).

**SD card for carts.** PICO-8 carts are tiny (.p8 text ≈ tens of KB, .p8.png = 32KB steganographic). Both Makerfabs boards have a microSD slot. On ESP32-S3 prefer **SDMMC (1-bit or 4-bit)** for speed, but watch pin conflicts with the parallel bus — on a pin-starved parallel board, **1-bit SD or SPI-mode SD** is the pragmatic choice. Carts are so small that SD speed is irrelevant; load the whole cart into RAM at launch.

### 5. Software Architecture & Development Plan

**Toolchain: use ESP-IDF (v5.1 or newer), not Arduino-ESP32.** ESP-IDF gives you `menuconfig` control over PSRAM mode/speed, cache line size, compiler optimization (-O2), FreeRTOS tick rate, and the `esp_lcd` DMA drivers you need — Arduino hides these. Espressif's own LCD guidance notes the RGB "screen drift" fix requires ESP-IDF ≥5.1 (arduino-esp32 v2.x is stuck on IDF 4.4). Use **PlatformIO with the ESP-IDF framework** if you want nicer dependency management, but the core is IDF. fake-08 builds with CMake/Make and pulls z8lua, lodepng, miniz, utf8-util, simpleini as submodules — all portable to an IDF component build.

**Recommended approach: port fake-08's core, replace its host layer.** fake-08 cleanly separates the platform-independent VM (`source/vm.h`, `p8GlobalLuaFunctions.h`) from per-platform `Host` implementations. You write a new `ESP32Host` that provides: framebuffer flush (your i80/RGB scale-and-blit), audio output (I²S callback pulling from the 22050Hz synth buffer), input (I²C expander or ADC ladder read → PICO-8 button bitmask), file access (SD via FatFs), and timing (`esp_timer`). This reuses fake-08's mature API coverage, cart parsing, and PNG decompression. **This is less effort and far higher compatibility than writing a runtime from scratch.** The alternative — building your own runtime on embedded Lua — means reimplementing the entire PICO-8 API surface and the PXA/legacy cart decompression, which is exactly what fake-08 already did over years.

**Dual-core / FreeRTOS design.** Pin **core 0** to the Lua VM + `_update`/`_draw` game logic; pin **core 1** to the display DMA feed and audio synthesis. Use a double-buffered handoff (the 128×128 indexed framebuffer) between them, plus a lock-free audio ring buffer. This matches PicoPico's "rendering happens on the second core" design that got Celeste to 9ms/frame.

**Cart loading.** Start with **.p8 plain text** — it is trivial to parse (sections: `__lua__`, `__gfx__`, `__gff__`, `__map__`, `__sfx__`, `__music__`). Add **.p8.png** (steganographic, then PXA-decompressed) second, reusing fake-08/zepto8's decoder. Precompile Lua to bytecode at load (or at build for a fixed cart) to cut parse time.

**Memory map plan.**
- **Internal SRAM (512KB):** Lua VM hot state (interpreter stack, current call frames), the 8KB indexed framebuffer (+ a second for double-buffering), audio DMA/synth buffers, the RGB565 line-scale buffer, DMA descriptors (DMA descriptors *cannot* live in PSRAM).
- **PSRAM (8MB):** the Lua heap (up to 2MB), cart code/data, sprite sheet, map, SFX/music data, and — only on the RGB panel path — the full-size RGB565 framebuffer.

**Phased, milestone-based plan with go/no-go gates:**

- **Phase 0 — This week, with the ILI9488 board + ESP-EYE you already own (no purchases):**
  1. Flash an ESP-IDF `esp_lcd` i80 example; get a **test pattern / color bars** on the ILI9488. *(Validates display output — the ESP-EYE is your fallback screen-output sanity check.)*
  2. Blit a static **128×128 indexed image, palette-expanded and 2×/3× integer-scaled**, and measure achieved fps over the i80 DMA path. **Go/no-go #1: ≥30fps full-frame.**
  3. Cross-compile **z8lua with fix32 for Xtensa** as an IDF component and run a micro-benchmark (e.g., a tight fixed-point loop and a `sin` sweep). **Go/no-go #2: Lua throughput within ~2–3× of the PICO-8 4M-inst/sec target.** *(This and #2 are the project-killers — do them first.)*
  4. Run a **trivial hand-written cart** (`hello world` / a moving sprite) end-to-end. *(~1–2 weeks of evenings.)*

- **Phase 1 — fake-08 core port (2–4 weeks):** Bring fake-08's VM into an IDF project, stub a minimal `ESP32Host`, get the BIOS/cart-browser and one simple real cart (e.g., a jam platformer) running from SD on the ILI9488. Add I²C-expander input and MAX98357A audio. **Go/no-go #3: a real cart playable at ≥30fps with sound.**

- **Phase 2 — 4-inch board + memory (2–3 weeks):** Port the host layer to the ST7701 RGB panel + N16R8; move the Lua heap fully into 8MB PSRAM; profile PSRAM bandwidth under the continuous RGB stream. Tune palette-scale into the PSRAM framebuffer. **Go/no-go #4: Celeste-class carts stable; decide 30 vs 60fps policy.**

- **Phase 3 — custom PCB + enclosure (4–8 weeks):** Only after software is proven. Choose final display (consider the 240×240 ST7789 tradeoff), lay out S3 module + expander + amp + charger + buttons, 3D-print an enclosure.

**De-risking order (most-likely-to-kill first):** (1) Lua-on-Xtensa speed, (2) PSRAM framebuffer bandwidth, (3) full-2MB-heap carts in PSRAM, (4) audio synthesis timing, (5) input pin budget. The first two are Phase 0.

**Fallback plans if 60fps isn't achievable:** (a) Run everything at **30fps** (PICO-8 supports `_update` at 30 natively; many carts are 30fps anyway). (b) **Restrict the supported-cart list** to ones that profile well, like fake-08's compatibility list. (c) **Precompile/optimize bytecode** and consider David's Lua→C AOT experiment (still incomplete). (d) **Change silicon:** the **ESP32-P4** (dual RISC-V @400MHz, 768KB SRAM, up to 32MB embedded PSRAM, a 2D Pixel-Processing-Accelerator and MIPI-DSI, single-precision FPU) is dramatically more capable for this workload — its PPA can offload scaling/blitting — at the cost of no built-in WiFi (needs a companion chip) and a less-mature ecosystem. The **RP2350** (with PSRAM) is yocto-8's stated preferred target but has less display/DMA muscle than the S3. If the S3 disappoints at 60fps, the P4 is the natural upgrade.

### 6. Enclosure / Mechanical / BOM (rough)

| Item | Part | Approx. price |
|---|---|---|
| MCU + display (prototype) | Makerfabs ESP32-S3 4-inch ST7701 480×480 (N16R8) | ~$45 |
| MCU + display (final option) | ESP32-S3-WROOM-1-N16R8 module + 2.4" ST7789 240×240 | ~$6 + ~$8 |
| Audio amp | MAX98357A breakout | ~$4 |
| Speaker | 3–4Ω, 1–4W (Makerfabs 4-inch includes one) | ~$2 |
| Input expander | MCP23017 or PCF8574 | ~$1.50 |
| Buttons | Tactile switches / silicone pad (NES-style pads work) | ~$3 |
| Battery | 2000mAh LiPo | ~$8 |
| Charger | TP4056 + protection, or onboard (4-inch has it) | ~$1 |
| microSD | 8–32GB | ~$5 |
| **Total (final custom)** | | **~$45–70** |

**Enclosure.** 3D printing (FDM/resin) is the right choice for one-offs and low volume; CNC only if you want metal. Layout options: **Game Boy-style** (screen top, D-pad + buttons below — natural for a 3.5"/4" screen in portrait-ish or landscape) vs **PSP/landscape** (screen center, controls flanking — better ergonomics for a 4" landscape panel). Adaptable open-source references: the PicoSystem form factor, and the many Raspberry Pi Zero handheld enclosures on Thingiverse/Printables can be re-scaled. Design the button pad around off-the-shelf NES-style silicone pads to avoid custom molding.

### 7. Licensing / Legal (brief)

- **The runtime is fine; distributing Lexaloffle's assets is not.** fake-08 and zepto8 are **clean-room reimplementations** (fake-08 is MIT-licensed) and are explicitly *"not related to or supported by Lexaloffle."* Reimplementing the PICO-8 API is generally accepted. What you must **not** ship is Lexaloffle's actual PICO-8 binary, its BIOS, or its bundled font data — build/use open equivalents.
- **Lexaloffle's stance is tolerant.** Joseph "zep" White has not pursued the many homebrew emulators; the community norm (echoed by fake-08, ROCKNIX, muOS) is to **encourage buying the official PICO-8** — priced at **$14.99** per Lexaloffle's page ("Get PICO-8 · $14.99 · Purchasing PICO-8 also gives you access to all future updates"). Devices ship with fake-08 preinstalled as a *default* (muOS/ROCKNIX on Anbernic/Miyoo handhelds) precisely because it needs no purchased binary.
- **Cart distribution is the sharper issue.** Many BBS carts are unlicensed or CC-BY-NC-SA; do not preload copyrighted carts on a product. Ship the device empty (or with only carts you have rights to) and let users add their own.
- **Commercial viability.** A **personal/open-source project is clearly safe.** A commercial product is riskier: precedent exists (handhelds ship fake-08), but you would want to (a) ship no Lexaloffle assets, (b) preload no unlicensed carts, and (c) ideally reach out to Lexaloffle. Keep this a personal project unless you are prepared to do that legal legwork.

## Recommendations

1. **This week (zero purchases):** On the ILI9488 board, get a test pattern up via ESP-IDF `esp_lcd` i80, then blit a scaled 128×128 indexed framebuffer and measure fps. In parallel, build z8lua-with-fix32 for Xtensa and benchmark it. These two results decide everything.
2. **Order the right module now:** an **ESP32-S3-WROOM-1-N16R8** dev board (8MB Octal PSRAM) for heap headroom — your planned 4-inch board already has it, but a bare N16R8 devkit is useful for isolated benchmarking. Also grab a **MAX98357A**, an **MCP23017**, and a **2.4" 240×240 ST7789** to evaluate the small-screen tradeoff.
3. **Port fake-08, don't rewrite.** Base the runtime on fake-08's VM and write only the `ESP32Host` layer. Start cart support with .p8 text, add .p8.png second.
4. **Commit to a 30fps baseline** with 60fps as a per-cart bonus. Split cores: Lua on core 0, display+audio on core 1.
5. **Decide the display before the PCB.** If Phase 0/2 shows the 480×480 RGB stream starves PSRAM bandwidth, switch the final build to a 240×240 ST7789 (better power, near-integer scaling, simpler DMA).

**Benchmarks/thresholds that change the plan:**
- If Phase-0 Lua benchmarks come in **>3–4× slower** than the PICO-8 target after fix32 + bytecode, plan for a 30fps-only device or jump to the **ESP32-P4**.
- If full-frame blit on the ILI9488 i80 path is **<20fps**, redesign around partial updates (retro-go style) or a smaller display.
- If real carts routinely exceed **~1.5MB Lua heap** and stutter from PSRAM latency, cap the supported-cart list.

## Caveats

- **The "9ms Celeste" figure is on an ESP32 (LX6 Wrover, 4MB PSRAM), not an ESP32-S3.** The S3 (LX7, same 240MHz) should be comparable or better, but this has not been publicly benchmarked on the S3 — treat it as a strong indicator, not a guarantee. *(Update 2026-07-14: we now have a first-party S3 measurement of raw z8lua throughput — see the update at the top; it lands ~2.5× under the PICO-8 target, inside the pass window, once `-fjump-tables` is set. A full-cart frame time on the S3 is still unbenchmarked.)*
- **No one has published a complete, polished ESP32-S3 PICO-8 handheld.** PicoPico (partial API coverage, no music, incomplete SFX), PeX Labs (early, no published FPS), and the lilka fork are the state of the art. You are doing genuinely new integration work.
- **PSRAM bandwidth numbers vary with config.** The ~84 MB/s figure is one forum measurement exceeding cache; with the 120MHz PSRAM option, careful cache-line tuning, and keeping hot data in SRAM, real results differ. Measure on your board.
- **The 4-inch board's RGB interface is a double-edged sword:** great for a big bright panel, but it streams the framebuffer continuously from PSRAM, competing with the Lua heap for bandwidth in a way the i80 ILI9488 does not.
- **fake-08's own limitations carry over:** imperfect noise-channel audio, some v0.2.2+ features (custom fonts, fill patterns) unimplemented, and `flip()`-based tweetcarts can be unstable. Your port inherits these.
- **fake-08's exact minimum RAM / whether it statically reserves the full 2MB Lua heap was not confirmed from source;** the fact that it runs on a 4MB-RAM Nintendo DS strongly implies dynamic heap growth rather than a fixed 2MB reservation, but verify this when you profile.
- **Legal tolerance is not a license.** Lexaloffle's current leniency toward homebrew runtimes could change, especially for a commercial product; nothing here is legal advice.