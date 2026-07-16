# ESP32-S3 PICO-8 Handheld — Refined Development Plan (v2, verified)

**Status:** planning. This is the tightened, decision-locked distillation of
[`pico-e32-runtime-feasibility.md`](design-specification/pico-e32-runtime-feasibility.md) after a
verification pass against primary sources (vendor spec tables, fake-08/z8lua source,
Espressif docs). It **supersedes** the original where they conflict; the original is
kept as the fuller evidence base. Corrections to the original are listed in
[§9](#9-corrections-to-the-original-plan).

**Scope lock:** this plan targets the **ESP32-S3** on the hardware on hand — the Makerfabs
**3.5" ILI9488** board (i80, 2 MB), the ordered Makerfabs **4.0" 480×480 ST7701** board
(RGB, 8 MB octal, onboard audio + charging), and the ESP-EYE camera. The alternative
"locked-60fps → ESP32-P4" path is documented separately in
[`pico-e32-silicon-decision.md`](design-specification/pico-e32-silicon-decision.md) and is the
fallback if the S3 misses the framerate bar (see [§3](#3-the-two-project-killers--phase-0-gono-go-gates)).

> **First plan / starting point: build entirely on the 3.5" ILI9488 board.** Phases 0–1
> use only the ILI9488 + ESP-EYE — get the runtime, display, input, and audio working on
> the i80 board first. The ordered 4.0" board is a **later** option (bigger screen, 8 MB,
> onboard audio/charging), deliberately **parked behind Gate #5** (§4/§8); it does not
> enter the plan until the runtime already works on the ILI9488. Everything below keeps
> the ILI9488 as the primary target; the 4" board is only ever discussed as "later."

---

## 1. TL;DR (what changed, and the plan in five lines)

1. **First device = the 3.5" ILI9488 board (N16R2 = 2 MB quad PSRAM, owner-verified), and
   2 MB is enough to start.** On the i80 path the ILI9488 holds the image in its **own
   panel GRAM**, so the ESP keeps **no full framebuffer in PSRAM** — the ~2 MB is free for
   the Lua heap. Most carts use a fraction of PICO-8's 2 MB Lua ceiling (PicoPico's real
   footprint was ~130 KB), so the large majority run comfortably. Only a cart pushing near
   the full 2 MB ceiling runs short of GC headroom — a *later* concern, already covered by
   the ordered 8 MB 4" board. **No new silicon needed to begin.**
2. **The display is not the bottleneck — the Lua interpreter is.** The ILI9488 over
   16-bit i80 clears ~130 fps theoretical (even 8-bit/10 MHz ≈ 32 fps). Pick the panel
   for *scaling cleanliness and ergonomics*, not FPS.
3. **fake-08 is confirmed a low-risk porting base on the VM axis** — its Lua heap grows
   dynamically (no static 2 MB reservation), the interpreter is bare-metal-clean, and
   all OS coupling sits behind one `Host` class. **Caveat: nobody has run fake-08 on an
   ESP32 yet** (the proven ESP32 PICO-8 runtimes are *tac08* and *PicoPico*). Phase 0
   exists to de-risk exactly this.
4. **Display: interface class matters more than screen size.** Primary render path =
   the **3.5" ILI9488 i80** board — GRAM-backed, so it's *structurally immune* to the RGB
   drift failure (2× = 256², the max clean integer scale on a 320-short-axis panel). The
   ordered **4.0" 480×480 ST7701 (clean 3× = 384², 8 MB, onboard audio/charging)** is the
   "complete handheld" candidate but its **RGB panel streams the framebuffer from PSRAM
   continuously with no hardware QoS → permanent screen drift when the CPU/Lua-heap
   contends for PSRAM.** It's promoted to target **only after a hardware drift soak test**
   (Gate #5). *The heavy carts that need its 8 MB are the ones most likely to drift.*
5. **De-risk before you design anything:** Phase 0 runs two benchmarks **on the ILI9488** —
   (A) a 2×-scaled 128×128 blit over i80, (B) z8lua+fix32 throughput on the LX7 — with hard
   go/no-go thresholds. The 2 MB board runs both fine (the Lua microbench uses a tiny heap).
   These two results decide the whole project.

---

## 2. Verified hardware — Makerfabs ESP32-S3 Parallel TFT w/ Touch (ILI9488, 3.5")

Sources: Makerfabs product page + wiki (fetched with a browser UA — both 403 to naïve
fetchers), and the vendor's own `config.h` / LovyanGFX board configs in the
[Makerfabs GitHub repo](https://github.com/Makerfabs/Makerfabs-ESP32-S3-Parallel-TFT-with-Touch).

| Subsystem | Verified detail |
|---|---|
| **MCU / memory** | **ESP32-S3-WROOM-1-N16R2** = 16 MB flash + **2 MB quad PSRAM** (owner-confirmed). *Makerfabs' current rev ships N16R8 = 8 MB octal; this unit is the earlier one.* Implication in §6/§7. |
| **Display** | ILI9488, **320×480**, **16-bit Intel-8080 (i80) parallel**, main clock up to **20 MHz**. Over parallel the ILI9488 uses **RGB565 (2 bytes/px)** — it avoids the notorious 3-byte-per-pixel SPI penalty. |
| **Backlight** | PWM on **GPIO45**. |
| **LCD data pins** | D0=47, D1=21, D2=14, D3=13, D4=12, D5=11, D6=10, D7=9, D8=3, D9=8, D10=16, D11=15, D12=7, D13=6, D14=5, D15=4. |
| **LCD control** | WR=18, RD=48, RS/DC=17, CS=46 (held permanently low in the vendor demo), RST=board reset. Bus is **shared with the SD card** (`bus_shared=true`). |
| **Touch** | **FT6236 capacitive**, I²C addr **0x38**, on **SDA=38 / SCL=39**; INT & RST **not connected** → touch is **polled**. |
| **microSD** | **SPI, 1-bit** (not SDMMC): CS=1, MOSI=2, MISO=41, CLK=42. |
| **Expansion** | One **Mabee** (Grove-compatible) connector = **1×I²C** (the *same* bus as the touch, GPIO38/39) **+ 1×GPIO (GPIO40, not ADC-capable)**. |
| **USB** | **Dual USB-C**: native USB (GPIO19/20) + USB-UART via **CP2104** (GPIO43/44). |
| **Onboard buttons** | Only **BOOT (GPIO0)** and **Reset** — no user/nav buttons broken out. |
| **Power** | **USB-C 5 V only. No battery, no charger onboard.** |

### The real constraint this board imposes: free GPIO ≈ **one**

After the 16-bit bus + PSRAM + SD + touch + USB/UART, **only GPIO40 is truly free**
(and it isn't ADC-capable, so an **analog resistor-ladder button scheme is out**).
GPIO43/44 are *reclaimable* if you flash/log over **native USB** instead of the CP2104
UART. Practical consequences, which shape the whole input/audio design in
[§5](#5-input-audio--power-for-this-board):

- **Buttons → I²C GPIO expander on the Mabee I²C bus** (0 extra GPIO; pick an address
  ≠ 0x38 so it doesn't collide with the FT6236 touch). This is the only clean path.
- **I²S audio needs 3 pins** → GPIO40 + reclaimed GPIO43/44, i.e. audio forces you onto
  native-USB flashing. It fits, but it's tight — and it's the strongest signal that
  **this board is a prototype, not the final handheld.**

---

## 2b. Verified hardware — Makerfabs 4.0" ST7701 480×480 (ORDERED)

The user ordered <https://www.makerfabs.com/esp32-s3-parallel-tft-with-touch-4-inch.html>
(module SKU **E32S3RGB40**). Verified from the product page/wiki + the vendor's own
config files in [github.com/Makerfabs/ESP32-S3-Parallel-TFT-with-Touch-4inch](https://github.com/Makerfabs/ESP32-S3-Parallel-TFT-with-Touch-4inch).
**This board consolidates memory + audio + power into one — but it uses an RGB panel (see
§4 for why that matters).**

| Subsystem | Verified detail |
|---|---|
| **MCU / memory** | **ESP32-S3-WROOM-1-N16R8** = 16 MB flash + **8 MB octal PSRAM** ✓ (solves the heap-size problem; no separate devkit needed). |
| **Display** | **ST7701S, 480×480**, **RGB565 (16-bit) parallel + 3-wire SPI** for register init. **No panel GRAM → continuous PSRAM scanout** (the §4 risk). Shipped **PCLK 14 MHz → ~49 Hz** refresh. |
| **Driver (already vendored)** | **LovyanGFX** (`components/LovyanGFX`, `1.2.25`) covers this panel: `Bus_RGB` + a dedicated `Panel_ST7701` class for the ESP32-S3 (`src/lgfx/v1/platforms/esp32s3/{Bus_RGB,Panel_RGB}.{hpp,cpp}`, `Panel_RGB.hpp:101`), **plus a ready-made config for this exact board** — `src/lgfx_user/LGFX_ESP32S3_RGB_MakerfabsParallelTFTwithTouch40.h` (Bus_RGB + Panel_ST7701, 480×480, 14 MHz, porches filled). So the Phase-2 "port the host to the RGB path" already has a driver: the **same library** that drives the 3.5" i80 board (`Bus_Parallel16`) drives this one — one API across the roadmap (it also does SPI panels). ⚠️ **The library does not dodge the §4 risk:** `Panel_ST7701` extends `Panel_FrameBufferBase`, i.e. exactly the continuous-scan-from-PSRAM-framebuffer model that Gate #5 exists to test. |
| **RGB pins** | R0-4 = 39,40,41,42,2 · G0-5 = 0,9,14,47,48,3 · B0-4 = 6,7,15,16,8 · **DE=45, VSYNC=4, HSYNC=5, PCLK=21** · 3-wire SPI init: CS=1, SCLK=12, MOSI=11. |
| **Backlight** | ⚠️ **Not PWM-dimmable as shipped** — requires a solder mod (add **R28 = 1 kΩ**, remove **R29**). Matters because backlight dimming is the #1 battery lever. |
| **Touch** | **GT911 capacitive**, I²C **SDA=17 / SCL=18**, INT=NC, RST=38 (this is also the Mabee I²C bus). |
| **Audio** | ✓ **MAX98357A I²S amp + onboard speaker** — BCLK=20, LRCK=46, DOUT=19. *Solves the audio pin-budget crunch.* |
| **microSD** | SPI: SCK=12, MISO=13, MOSI=11, CS=10 (SCK/MOSI shared with the 3-wire display SPI). Board advertised with an onboard 16 GB card. |
| **Power** | ✓ **Onboard LiPo charger + battery connector.** *Solves power/charging.* |
| **Expansion / free GPIO** | Mabee (1×I²C on 17/18 + 1×GPIO). The RGB bus + audio + SD + touch consume **nearly every usable GPIO** → **buttons must go on an I²C expander** on the 17/18 bus (address ≠ GT911). |

**Net:** this board removes three BOM lines (8 MB module, audio amp+speaker, charger) and
the audio/power pin-budget problem — at the cost of the RGB display-drift risk that Gate #5
(§8) must clear on hardware before it's trusted as the target.

---

## 3. The two project-killers — Phase 0 go/no-go gates

Everything hinges on two questions, both answerable on the hardware you already own,
**with zero purchases**. Do these *before* buying parts, choosing a final display, or
laying out a PCB. Run the two tracks in parallel.

### Track A — Display path (`esp_lcd` i80 → ILI9488)

| Step | Action |
|---|---|
| A1 | Flash an `esp_lcd` i80 example targeting this board (start from [`atanisoft/esp_lcd_ili9488`](https://github.com/atanisoft/esp_lcd_ili9488) or a LovyanGFX `Bus_Parallel16` config using the pin map above). Get **color bars** on the panel. Confirm via an **ESP-EYE camera capture**. |
| A2 | Blit a static **128×128 indexed** test image → palette-expand to RGB565 → **integer-scale 2× to 256×256**, centered with black borders → push over i80 **DMA**. Time the sustained flush loop with `esp_timer`. |

**🚦 GATE #1 — ≥30 fps sustained** for the 256×256 scaled full-refresh.
*Expected: comfortably met (bus theoretical ≈130 fps at 16-bit/20 MHz; ≈32 fps even at
8-bit/10 MHz).* If <30 fps, the problem is pclk/DMA config, not the panel — investigate
before proceeding.
**Evidence:** FPS printout **+** an ESP-EYE photo showing the image sharp with correct
borders.

### Track B — z8lua / Lua throughput on the LX7 (the *actual* risk)

| Step | Action |
|---|---|
| B1 | Build **`libs/z8lua`** (`LUA_NUMBER = z8::fix32`, Lua 5.2) as an **ESP-IDF component**, **compiled as C++** — `fix32` is a C++ type, so it can't build as C — with **`-DLUA_USE_LONGJMP`** so `LUAI_THROW` uses setjmp/longjmp and C++ exceptions can stay disabled. It opens no io/os/loadlib libs and loads carts from memory — needs only `malloc` + `setjmp/longjmp`. **Done & host-verified — see [`firmware/pico-e32-luabench/`](../firmware/pico-e32-luabench/).** |
| B2 | Run a fixed micro-benchmark: (i) a tight fixed-point arithmetic loop, (ii) a `sin()` sweep (~32k calls), (iii) a table-churn/alloc loop — **timed with the Lua heap in PSRAM (`heap_caps` SPIRAM allocator) vs internal SRAM**, via `esp_timer`. |

**🚦 GATE #2 — z8lua sustained throughput within ~2–3× of PICO-8's ~4.2 M VM-inst/sec
target** (PICO-8's virtual CPU = 139,810 cycles/frame @60 fps ≈ 4.19 M inst/s), i.e. a
representative frame's Lua work must fit in **≤33 ms (30 fps floor)**, ideally ≤16.6 ms.
*Reference: PicoPico runs Celeste at **~9 ms/frame on a slower ESP32 LX6** — the S3
(LX7, 5.54 CoreMark/MHz, +FPU +SIMD) is a safe **lower bound** of that.*
**Evidence:** measured inst/s and ms/frame, plus the **SRAM-vs-PSRAM delta** (this tells
you how much hot state must stay in internal SRAM).

> **✅ Measured 2026-07-14 (ILI9488 N16R2, S3):** Gate #2 **passable** — fixed-point ≈ 0.29 M
> inner-iter/s ≈ **~1.6 M VM inst/s, ~2.5× under** the 4.2 M target, inside the ~2–3× window.
> **The governing lever was a build flag:** ESP-IDF's `-fno-jump-tables` made opcode dispatch a
> linear compare-chain (~15× tax); **`-fjump-tables` gives ~2.8–3.1×** and is what clears the gate
> (one line in the z8lua component, no source edit). Ruled out (≤6% each): larger I-cache, QIO
> flash, IRAM, `always_inline`, and the fix32 `double→float` switch (z8lua math is already
> fixed-point). SRAM-vs-PSRAM: identical for compute; but the alloc-heavy case **OOM'd on internal
> SRAM (369 KB free)** → the Lua heap must live in PSRAM. Full detail:
> [`worklog/2026-07-14-phase0-gate2-luabench.md`](worklog/2026-07-14-phase0-gate2-luabench.md).

> **PSRAM note (not a blocker):** on the i80 path the display never touches PSRAM, so the
> interpreter has the whole PSRAM port to itself — and with the hot Lua state kept in SRAM,
> the 2 MB *quad* part is a perfectly good Gate #2 platform. Its bandwidth is ~half the
> ordered 4" board's octal, so a *pass* here is conservative; when the 4" board arrives you
> can re-confirm the definitive octal numbers on it — **no purchase needed.**

### Phase 0.5 — trivial end-to-end cart (integration smoke test)

Port a *minimal* `ESP32Host` (only `drawFrame` + timing; no audio/input yet, read BOOT
as a stand-in button) and run a hand-written `.p8` (a moving sprite) through fake-08's
VM from flash.
**🚦 GATE #3 — a trivial cart renders on the panel at ≥30 fps.** Evidence: ESP-EYE
capture of the moving sprite + FPS.

> If Gate #2 fails badly (heavy carts can't clear 30 fps even after fix32 + bytecode
> precompile + SRAM-arena tuning), that is the trigger to switch silicon to the
> **ESP32-P4** — see the sibling silicon-decision doc.

**Phase-0 status** (firmware in [`../firmware/`](../firmware/); ESP-IDF v5.4.2 vendored at
`vendor/esp-idf`): both Phase-0 apps now **compile clean for esp32s3** (`make build`).
`pico-e32-luabench` (Gate #2) also **builds + runs on the host** — z8lua + fix32 confirmed
working; the on-device build measures the SRAM-vs-PSRAM heap delta. `pico-e32-display-test`
(Gate #1, i80 blit + FPS) compiles but is **not yet run on hardware**. Findings from actually
building it: (1) z8lua must compile as **C++** (fix32 is a C++ type) with `-DLUA_USE_LONGJMP`;
(2) it exposed a real `fix32.h` portability bug on xtensa (plain `int` unconstructible where
`int32_t == long`) — patched; (3) z8lua numbers are **16.16 fixed point (range ±32768)**, so a
`for i=1,2000000` loop overflows — keep bounds < 32767.

---

## 4. Display — the decision that changed (interface matters more than size)

**The near-term plan is the 3.5" ILI9488; the 4" board is a later option.** A verification
pass *confirmed* that ordering — and showed *why* the i80 board (not the bigger RGB one) is
the right primary render path. (4" spec: [§2b](#2b-verified-hardware--makerfabs-40-st7701-480480-ordered).)

**Two framing facts:**
1. **Scaling geometry** (unchanged): a clean integer square scale needs the panel's short
   axis ≥ scale × 128.
2. **Display *interface class* is the real risk axis** — not screen size. Panels with
   their own GRAM driven over **i80/SPI** take *bounded, on-demand* transactions.
   **RGB** panels have *no* frame memory: the ESP32-S3 must **stream the whole framebuffer
   out of PSRAM continuously via GDMA**, and the S3 has **no hardware QoS** to protect that
   stream. When the CPU also touches PSRAM (framebuffer writes, code fetch, **Lua-heap
   spill**, SD/flash I/O), it "stretches" the GDMA and causes **permanent screen drift**,
   documented by Espressif and reproduced on this exact ST7701 480×480 panel.

| Panel | Short axis | Max clean integer scale | Interface | Drift-failure class? |
|---|---|---|---|---|
| **3.5" 320×480 ILI9488 i80** (owned) | 320 | **2× = 256²** (3×=384 won't fit) | i80 (GRAM) | **No — structurally immune** |
| 2.0–2.4" ST7789 240×240/240×320 SPI | 240 | **1× = 128² only** | SPI (GRAM) | No |
| **4.0" 480×480 ST7701** (ordered) | 480 | **3× = 384²** (best fill, ~2.26" sq) | **RGB (no GRAM)** | **Yes — PSRAM-contention drift** |

### The locked call

- **Primary render path → the 3.5" ILI9488 i80 board.** It *cannot* exhibit the RGB drift
  failure by construction: bounded per-frame pushes to the panel's GRAM, no continuous
  PSRAM scanout, deterministic FPS. **The full runtime is built here.** Because the i80
  path keeps no framebuffer in PSRAM, the ~2 MB is free for the Lua heap, so it runs the
  large majority of carts; only carts near PICO-8's full 2 MB Lua ceiling are constrained
  (a later concern — see §6).
- **4.0" RGB board → the "complete handheld" candidate, promoted to primary ONLY after a
  hardware drift soak test passes** (Gate #5, §8). It has everything else you want —
  8 MB octal (heap headroom), onboard MAX98357A audio + speaker, LiPo charging, and the
  only clean **3× = 384²** big square image. **But its risk is cart-dependent and
  perverse:** light carts keep their hot heap in the 512 KB SRAM and render clean; the
  **heavy carts that actually need the 8 MB must spill the heap to PSRAM — the exact
  random-access traffic that drifts the screen.** No 480×480 ST7701-on-S3 board has a
  *published sustained full-frame game* FPS; they ship as LVGL HMIs (partial/dirty
  redraw), which is a genuine red flag for a full-frame emulator.
- **If the RGB drift can't be tamed** for your cart set, the fidelity endgame is a custom
  build: an **N16R8 (8 MB) module + a GRAM-backed i80/SPI panel** — sacrificing the 4"
  RGB's size/integration for display stability. Or ship the 4" board for light/moderate
  carts with a documented heavy-cart caveat.
- **Rejected: 240-class ST7789** — forces a postage-stamp 1× or blurry non-integer scale.

**Bottom line:** *screen size was never the hard part; the RGB-vs-GRAM interface is.* The
big 8 MB board is worth pursuing, but it has to earn "target" status on the bench — it
doesn't get it by spec.

---

## 5. Runtime decision — port fake-08, replace its host layer

**Base: fake-08** (`jtothebell/fake-08`, MIT) — highest PICO-8 API coverage of the open
runtimes, and verified from source to be a clean port target. **Honest caveat:** no one
has run fake-08 on an ESP32 specifically; the proven ESP32 PICO-8 runtimes are **tac08**
(PeX Labs ported it to FreeRTOS/ESP32) and **PicoPico** (a from-scratch z8lua runtime,
Celeste ~9 ms/frame on ESP32-Wrover). fake-08 is the better *base* (coverage + clean
`Host` seam); Phase 0 Gate #2 is what converts "should work" into "does work."

**What you implement — one `ESP32Host` (fake-08's `Host` is a single concrete class in
`source/host.h`; each platform just links its own definitions):**

| Host responsibility | fake-08 signature | ESP32 implementation |
|---|---|---|
| Framebuffer flush | `drawFrame(uint8_t* picoFb, uint8_t* paletteMap, uint8_t drawMode)` — `picoFb` = native **128×128 4-bpp indexed** (8 KB) | nibble→RGB565 line expansion + 2× scale → i80 DMA |
| Audio | `Audio::FillAudioBuffer(...)` @ **22050 Hz S16**; use the **pull/poll** path (`shouldFillAudioBuff`/`getAudioBufferPointer`/…) | ESP-IDF **I²S** feeder task |
| Input | `InputState_t scanInput()` → 8-bit mask (LEFT/RIGHT/UP/DOWN/O/X/PAUSE) | read I²C expander → OR the bits |
| File/cart | `getFileContents`, `listcarts`, `saveCartData`, … | SD (FatFs) / LittleFS via VFS |
| Timing | `setTargetFps`, `waitForTargetFps`, `deltaTMs` | `esp_timer` + `vTaskDelay` |

**Concrete port notes (verified from source):**
- **No CMake in fake-08** (per-platform GNU Makefiles only) → **author your own IDF
  component `CMakeLists.txt`** listing `source/*.cpp`, `libs/z8lua/*` **compiled as C++**
  (fix32 is a C++ type; add `-DLUA_USE_LONGJMP`), `libs/{lodepng,miniz,simpleini}`. Define
  `LODEPNG_NO_COMPILE_DISK`. Only **z8lua** is a
  submodule; lodepng/miniz/simpleini are vendored; `utf8-util` is a dangling Makefile
  reference (absent, unused) — ignore it.
- **PSRAM heap:** replace `luaL_newstate()` at **`source/vm.cpp:300`** with
  `lua_newstate(psram_alloc, …)` where `psram_alloc` uses
  `heap_caps_malloc/realloc(…, MALLOC_CAP_SPIRAM)`. Keep in **internal SRAM**: the 8 KB
  `picoFb` (double-buffered), audio DMA buffers, the RGB565 line buffer, and **all DMA
  descriptors** (these cannot live in PSRAM). **On the 4" RGB board this SRAM placement is
  doubly load-bearing:** keeping the **hot Lua state + GC nursery in SRAM** (not just the
  framebuffer) is the shared mitigation for *both* the speed gate (#2) and the RGB-drift
  gate (#5) — random-access heap walks in PSRAM are what "stretch" the RGB GDMA and drift
  the screen. On the RGB path also enable bounce buffers, `num_fbs=2`,
  `CONFIG_LCD_RGB_RESTART_IN_VSYNC`, and `CONFIG_SPIRAM_XIP_FROM_PSRAM`.
- **Dual-core split** (mirrors PicoPico): Lua VM + `_update`/`_draw` on **core 0**;
  display DMA feed + audio synth on **core 1**. Hand off the indexed framebuffer
  double-buffered; lock-free audio ring.
- **Bytecode precompile** (PicoPico's `to_c.py` approach: parse **112 ms → 18 ms**) is
  high-value; decide build-time (fixed cart) vs load-time (SD cart browser) in Phase 1.
- **fix32 does the heavy lifting for free:** all cart arithmetic is 16.16 fixed-point, so
  it never touches the FPU; audio synth is cheap once fix32 (**25 ms → 2 ms per SFX**).

**Licensing (unchanged, confirmed):** fake-08 is a clean-room MIT runtime that ships
**none** of Lexaloffle's assets — fine to build and even redistribute the binary. The
real exposure is **bundling carts you don't have rights to** (most default to
all-rights-reserved). **Ship empty; users add their own carts.** Keep it a personal /
open-source project; a commercial product needs its own legal legwork. Full detail in
the licensing report from the earlier research.

---

## 6. PSRAM — 2 MB on the ILI9488 is enough to start (and for most carts)

**Settled (owner-verified): the ILI9488 board carries `ESP32-S3-WROOM-1-N16R2` = 16 MB
flash + 2 MB *quad* PSRAM.** (Makerfabs' *current* rev of this board ships N16R8/8 MB-octal
and the live page says so, but this unit predates that swap — trust the module marking, not
the page.)

**Why 2 MB is fine for the first device — the i80 path uses no PSRAM framebuffer.** The
ILI9488 has its **own on-panel GRAM**: you push scaled pixels to it over i80 (through a
small SRAM line buffer) and the panel retains the image. Nothing streams from PSRAM, and
you never hold a full 320×480 RGB565 buffer on the ESP. So the entire ~2 MB is available to
the **Lua heap + cart data**:

- PICO-8's Lua ceiling is 2 MB, but **most carts use a small fraction** of it (PicoPico's
  measured footprint was ~130 KB). The large majority run comfortably in 2 MB.
- Only a cart pushing near the **full 2 MB Lua ceiling** runs short of GC headroom on a
  2 MB part. That's a minority of the library and a **later** concern.

**The 8 MB upgrade is already in hand — for later.** The ordered 4" board is N16R8 (8 MB
octal), so if/when heavy-cart headroom matters, you already have the memory (its *display*
caveat is separate — §4). **You do not need to buy any module.** Keep the **hot Lua state +
GC nursery in the 512 KB internal SRAM** regardless — that's the main speed lever (Gate #2)
and keeps PSRAM traffic low.

> **The framebuffer-in-PSRAM problem is an RGB thing, not an ILI9488 thing.** The *only*
> reason PSRAM size/bandwidth gets fraught is the **RGB** 4" path, which must hold the full
> framebuffer in PSRAM and stream it continuously (§4). The ILI9488 i80 path has neither
> pressure. Don't let the RGB board's memory story bleed into the ILI9488 plan.

---

## 7. BOM — what to buy for the ILI9488 build

**Owned / ordered — do not buy:**
- Makerfabs **3.5" ILI9488** board (2 MB, i80) — **the first device**; the entire near-term
  build runs on it.
- Makerfabs **4.0" ST7701** board (~$32, ordered) — *later* option; N16R8 8 MB + onboard
  MAX98357A amp/speaker + LiPo charger + 16 GB SD.
- **ESP-EYE** — screen-capture verification camera.

**To turn the ILI9488 into a working handheld, buy (small list):**

| Part | Price | Phase | Role |
|---|---|---|---|
| **I²C GPIO expander** — PCF8574 (8 I/O, $3–5) or MCP23017 (16 I/O, $5–7) | $3–7 | 1 | **buttons** on the ILI9488 I²C bus (**SDA=38 / SCL=39**), address ≠ 0x38 (FT6236). The only clean input path (≈1 free GPIO). |
| Tactile buttons (12 mm 10-pack) or NES silicone pads | $2.50–9 | 1 | D-pad + O/X + menu (7 inputs) |
| **MAX98357A I²S amp + 4 Ω speaker** | $3–10 | 1 | audio out — **the ILI9488 board has none**; BCLK/LRCK/DIN on GPIO40 + reclaimed 43/44 |
| microSD 8–32 GB | $8–10 | 1 | cart storage (reuse a spare if you have one) |
| LiPo 2000 mAh + TP4056 charger | $13–15 | 2 | only for a *portable* ILI9488 build (board is USB-powered, no onboard charger) |
| MAX17048 fuel gauge | $6–13 | opt | battery % readout |

**Tethered Phase-0/1 bring-up ≈ $6–16** (I²C expander + buttons; +~$8 for audio if you want
sound early). A portable ILI9488 build adds the LiPo + charger (~$14).

**The 4" board, later, deletes most of this** — amp, speaker, charger, and 16 GB SD are all
onboard (its one gotcha: backlight PWM needs an R28/R29 solder mod). But that's Phase 2;
none of it is needed to start on the ILI9488.

**Dropped from the original plan:** the separate "N16R8 devkit" (2 MB carries the near-term
plan, and the 4" board already provides 8 MB for later), and the "240×240 ST7789 eval"
(barely exists at that size/res; we recommend against the 240-class regardless).

---

## 8. Phased plan (with gates)

| Phase | Board | Goal | Exit gate |
|---|---|---|---|
| **0** | 3.5" | Track A: 2×-scaled i80 blit FPS. Track B: z8lua+fix32 throughput on the LX7 (SRAM vs PSRAM). **Both on the ILI9488**; re-confirm Track B's octal numbers on the 4" board when it arrives (no purchase). | **#1** ≥30 fps blit **and #2** Lua within ~2–3× of PICO-8 budget (≤33 ms/frame). |
| **0.5** | 3.5" | Minimal `ESP32Host` (draw+timing), trivial hand-written cart from flash — on the **safe i80 path**. | **#3** trivial cart ≥30 fps on panel (ESP-EYE-verified). |
| **1** | 3.5" | Full `ESP32Host`: I²C-expander input, I²S pull-audio @22050 Hz (external MAX98357A), SD cart loading. Run a real moderate cart. | **#4** a real cart playable ≥30 fps with sound + input; set 30-vs-60 fps policy from measured frame times. |
| **2 — RGB validation** | 4.0" | Port the `ESP32Host` to the ST7701 RGB path (esp_lcd RGB, bounce buffers, hot heap in SRAM, PSRAM allocator). Use the 4" board's onboard audio + charging. **Run the drift soak test (Gate #5).** | **#5** (below) — decides whether the 4" board is the target or the 3.5"-class i80 path wins. |
| **3** | final | Enclosure + (only if going custom) a PCB: N16R8 module + the display that survived Gate #5. | Final hardware locked. |

### 🚦 GATE #5 — the 4" RGB drift soak test (must pass on real hardware, not arithmetic)

The 4" board becomes the trusted target **only if all three hold** (from the primary-source
failure analysis):

1. **Sustained ≥30 fps full-frame for several minutes, no drift**, with bounce buffers +
   `CONFIG_LCD_RGB_RESTART_IN_VSYNC` + `CONFIG_SPIRAM_XIP_FROM_PSRAM`, at a PCLK that also
   stays above the IPS flicker floor (~40 Hz ≈ 11.4 MHz) — and note this sits *near* the
   ~12 MHz FATFS-safe ceiling measured on this exact panel, so verify the window is real.
2. **The load-bearing test:** a cart whose Lua heap is *forced to spill to PSRAM* (~1.5–2 MB)
   still holds the bounce-buffer refill deadline during `_update`/`_draw` — i.e. the screen
   doesn't drift on heavy carts. This is the one assumption that "keep the hot set in SRAM"
   can't fully cover.
3. **A live SD cart-load and a mid-game SD/flash access** with the display running cause no
   drift (or a temporary PCLK drop during I/O is invisible).

If Gate #5 fails, the 4" board ships only for light/moderate carts (documented caveat), and
the fidelity endgame is a **custom N16R8 + GRAM-backed i80/SPI panel** build.

**De-risk order (most-likely-to-kill first):** (1) z8lua-on-LX7 throughput [Gate #2],
(2) **RGB scanout↔PSRAM drift on the 4" board** [Gate #5], (3) full-2 MB-heap carts in
PSRAM, (4) audio-synth timing, (5) input wiring. **(1) is Phase 0; (2) is Phase 2 and is
the gate that decides the final hardware.**

**Fallbacks if 60 fps proves unreachable:** run a **30 fps baseline** (PICO-8 natively
supports it; many carts are 30 fps); restrict the supported-cart list to ones that
profile well; adopt bytecode precompile + an SRAM-arena allocator + 120 MHz PSRAM (note
its thermal-calibration caveat); or, as the nuclear option, **switch to the ESP32-P4**
(≈2× single-thread Lua headroom + ~2.2× PSRAM bandwidth) per the silicon-decision doc.

---

## 9. Corrections to the original plan

The original [`pico-e32-runtime-feasibility.md`](design-specification/pico-e32-runtime-feasibility.md) is
sound in outline; these specific points are wrong or outdated and are fixed above:

1. **Board PSRAM:** the original's **2 MB** figure is **correct for this unit**
   (owner-confirmed N16R2; Makerfabs' *current* rev of the same board is N16R8/8 MB). But
   the "2 MB is too small, order a module" framing is **overstated for the i80 path**: the
   ILI9488 holds no framebuffer in PSRAM (it has its own GRAM), so ~2 MB is free for the Lua
   heap and runs the large majority of carts. **No module purchase needed** — and 8 MB
   headroom is already in hand via the ordered 4" board, for later (§6).
2. **Phantom "4-inch board you have planned":** you never specified one; the original
   treated a Makerfabs 4" ST7701 board as an owned/committed production target. It's a
   *candidate* for the dedicated build (§4/§7), not a given.
3. **Display scaling:** original says the 3.5" panel does "3× = 384×384." **False** for a
   square 128×128 image — the panel's short axis is 320, so **max clean integer = 2× =
   256²**. 384² only fits the 480×480 panel.
4. **fake-08 heap:** original hedged the "does it statically reserve 2 MB?" question. **It
   does not** — stock `luaL_newstate()` grows dynamically; no 2 MB cap is enforced in the
   source. State as resolved.
5. **fake-08 build/deps:** original says "builds with CMake/Make and pulls z8lua,
   lodepng, miniz, utf8-util, simpleini as submodules." **Wrong:** no CMake (GNU
   Makefiles only), **only z8lua** is a submodule (others vendored), `utf8-util` is a
   dead reference. You author your own IDF component.
6. **fake-08 "runs on a 4 MB Nintendo DS":** there is **no** original-DS port; the
   platform list is 3DS/Switch/Wii U/Vita/Miyoo/bittboy/etc. The dynamic-heap conclusion
   now rests on the *source*, not that anecdote.
7. **"MCP73831 charger":** as a breakout it's ~$11; a **TP4056** ($1–2) is the better
   Phase-1 default.
8. **"2.0–2.4" 240×240 ST7789" eval part:** essentially unobtainable at that size/res,
   and we recommend against the 240-class regardless — dropped.
9. **Display reversal (biggest change):** the original (and this doc's v1) named the
   **4.0" 480×480 ST7701 RGB** the dedicated-build target, "contingent on a bandwidth
   test." A primary-source verification pass shows that framing understates the risk:
   RGB panels stream the framebuffer from PSRAM **continuously with no hardware QoS**, and
   the S3 has a *documented* permanent-screen-drift failure when the CPU/Lua-heap contends
   for PSRAM — reproduced on this **exact** ST7701 480×480 panel (esp-bsp #570), and
   perverse because heavy carts (which need the 8 MB) spill the heap to PSRAM and trigger
   it. **The i80 board is structurally immune** and is now the primary render path; the 4"
   board must pass a hardware drift soak test (Gate #5) before it's the target. *Interface
   class (GRAM i80/SPI vs RGB) matters more than screen size.*
10. **4" board's onboard hardware is confirmed real** (was "verify" in v1): 8 MB octal
    PSRAM, MAX98357A amp + speaker, LiPo charger, 16 GB SD. This removes the separate
    N16R8-devkit, audio-amp, speaker, and charger BOM lines — but the backlight is **not**
    PWM-dimmable without an R28/R29 solder mod.

---

## 10. Open questions still to resolve

- **z8lua+fix32 on the actual S3 (LX7) is unbenchmarked** — every measured number is on
  LX6 (PicoPico) or ARM11 (fake-08/3DS). This *is* Gate #2; run it on hardware.
- **PSRAM GC-walk latency:** whether IDF's automatic PSRAM-spill malloc suffices, or a
  dedicated `heap_caps` PSRAM allocator + keeping GC-hot objects in SRAM is required, is
  unmeasured (the ~84 MB/s cache-miss ceiling could make GC over a large PSRAM heap slow).
- **fake-08 audio pull-path buffering contract** (block size, mono/stereo, timing) should
  be read from a console host (e.g. `platform/switch` or `platform/3ds`) before wiring
  I²S, to avoid underruns.
- **Bytecode-precompile strategy** (build-time fixed cart vs load-time SD browser) — a
  product choice affecting the 112→18 ms parse win.
- **Gate #5 (RGB drift) is unresolved by research** — the primary sources prove the risk
  is real and cart-dependent, but *whether the mitigations hold for heavy PSRAM-heap carts
  on this specific panel* is only answerable by the hardware soak test. No published
  480×480 ST7701-on-S3 sustained full-frame game FPS exists to lean on.
- **Is there a PCLK window that is simultaneously above the ~40 Hz IPS flicker floor and
  below the ~12 MHz FATFS-safe ceiling** on the 4" panel? The two constraints nearly
  collide (~11.4 MHz vs ~12 MHz) — confirm on hardware.

---

## Sources (load-bearing)

- Makerfabs ILI9488 board — product page & wiki (current page lists N16R8/8 MB, but *this
  unit* is the older N16R2/2 MB; pinout, FT6236, SD, Mabee): <https://www.makerfabs.com/esp32-s3-parallel-tft-with-touch-ili9488.html> ·
  <https://wiki.makerfabs.com/ESP32_S3_Parallel_3.5_TFT_with_Touch.html> ·
  <https://github.com/Makerfabs/Makerfabs-ESP32-S3-Parallel-TFT-with-Touch>
- ILI9488 i80 driver + RGB565-over-parallel + FPS: <https://github.com/atanisoft/esp_lcd_ili9488> ·
  <https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/i80_lcd.html>
- ESP32-S3 datasheet (LX7, 1329.92 CoreMark dual-core / 5.54 CM/MHz, FPU+SIMD, 512 KB
  SRAM): <https://documentation.espressif.com/esp32-s3_datasheet_en.pdf>
- fake-08 source (Host seam, dynamic heap `vm.cpp:300`, no CMake, deps): <https://github.com/jtothebell/fake-08>
  · z8lua (fix32 16.16, Lua 5.2, setjmp/longjmp): <https://github.com/jtothebell/z8lua>
- PicoPico (ESP32 proof: Celeste ~9 ms/frame, bytecode 112→18 ms, fix32 synth 25→2 ms):
  <https://github.com/DavidVentura/PicoPico> ·
  <https://blog.davidv.dev/posts/pico8-console-part-2-performance/>
- ESP-IDF RGB LCD (bounce buffers / PSRAM contention, for the 480×480 path): <https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html>
- Full evidence base: [`pico-e32-runtime-feasibility.md`](design-specification/pico-e32-runtime-feasibility.md),
  [`pico-e32-silicon-decision.md`](design-specification/pico-e32-silicon-decision.md).
