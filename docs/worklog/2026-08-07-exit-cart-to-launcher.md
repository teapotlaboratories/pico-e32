# 2026-08-07 — Exit a running cart back to the launcher (`IN-6`)

> **Running log — appended as the work happens.**

Goal: make the cart launcher a launcher. Today, once you pick a cart there is no way back to the carousel short
of power-cycling the board.

## 1. The gap, and how it surfaced

Found by being asked a plain product question — "when in the game, how do I exit to the main menu?" — and not
knowing the answer. Reading the code rather than guessing:

- `main.cpp:639` → `vm->GameLoop(); /* fake-08's own loop; never returns */`
- the deck's **MENU** button → `INPUT_PAUSE` (`board.cpp:541`) → `Vm::togglePauseMenu()`, which flips a flag,
  saves draw state and pauses audio — **and nothing renders menu items or acts on them**
- upstream leaves the intent behind, commented out: `vm.cpp:1247  //todo: pause menu here, but for now just load bios`
- `LoadBiosCart()` is only ever reached on a *cart load error*, never from a pause

So MENU freezes and silences the cart; pressing it again resumes. There is no exit. A launcher holding 3145
carts plays exactly one per boot.

Worth recording: I had internalised this while scripting the screen captures — every capture run took the
in-game shot **last**, because the board had to be reset afterwards — and never wrote it down as a defect. The
constraint was visible in my own tooling for days.

## 2. Two implementations that do not work, and why

**A watcher task polling for the gesture.** `input_poll()` is **destructive** on the serial backend: it drains
the UART / USB-JTAG buffer and decrements the per-key hold counters. A second caller would consume bytes the VM
never sees, so a background watcher would silently eat input. (The touch backend would merely double the I²C
traffic, but the serial one is fatal.)

**Checking from the app.** `app_main` is blocked inside `GameLoop()`, which never returns, so there is nowhere
in `main.cpp` for a per-frame check to live.

That leaves exactly two per-frame code paths: `ESP32Host::scanInput()` — architecturally the *right* home, since
the Host owns the device and a long-press is a device gesture, but it lives in the **fake-08 submodule** and so
costs a separate repo PR plus a gitlink bump — and `input_poll()` itself, which is ours.

## 3. Approach

A shared helper in `components/input`, called from inside each backend's `input_poll()`, counting consecutive
polls with `INPUT_PAUSE` held; past ~1.2 s it reboots. The launcher is ~1.8 s from reset with the SD already
mounted and the radio off, so **restart is the return path** — no VM unwind, no submodule change, no risk of
half-torn-down state.

Long-press rather than a tap, deliberately: a tap must keep meaning PICO-8's pause.

## 4. Built, and two things the first attempt got wrong

**Armed in the wrong place.** `input_exit_enable(true)` first went in beside `vm->GameLoop()` — but `main.cpp`
has **five** mutually-exclusive frame loops (`MEASURE_FPS` / `TELEMETRY` / `FB_DUMP` / `GC_MANUAL` / shipped),
so the gesture existed only in the shipped build. That is the worst version of the bug: the dev builds are
exactly where it gets exercised. Now armed **once, above the whole ladder** — "the cart owns the machine from
here" is true of every branch.

Caught only because the first hardware test ran the `FB_DUMP` loop rather than the shipped one: `-D` defines are
**sticky in the CMake cache**, so a build asking for `LAUNCHER=1 INPUT_BACKEND=serial` still had `FB_DUMP=1`
from an earlier capture run. `make fullclean` between config changes, as the Makefile's own note says.

**A poll count was the wrong unit.** The threshold started as 72 consecutive polls ("~1.2 s at 60 Hz"), which
silently changes length in any build whose loop is not 60 Hz — and four of the five are not. Now wall-clock via
`esp_timer_get_time()`.

## 5. Verified — and precisely how far

```
launch /sdcard/[Action-Adventure]/2-minute Picovania.p8.png
entering GameLoop (hold MENU to return to the launcher)
W input.exit: MENU held 30 ms — returning to the launcher (restart)
rst:0xc (SW_CPU_RESET)
carousel: carousel layout ...          <- back in the launcher
```

The full path works: armed at hand-off → gesture detected in the input layer → restart → carousel.

**The 1.2 s duration itself is NOT verified on hardware, and cannot be over serial.** The USB-Serial-JTAG
transport batches input at roughly 1 Hz here — sending `p` every 40 ms, the backend saw **5 bytes, ~1 s apart**,
each giving a 6-frame hold. A held button is unrepresentable over that link, so the run above used a temporary
30 ms threshold to prove the mechanism, and the real 1200 ms was restored afterwards. What is verified is
detect → restart → launcher; what is not is the feel of the hold. That needs a finger on the deck with the
touch backend, where a held press polls continuously — **an owner check, not something the bench can do.**

Also unverified for the same reason: that a *short* tap still reaches the VM as PICO-8's pause rather than
triggering the exit. The code path is the same `held & INPUT_PAUSE` the VM already consumes and the timer simply
does not reach 1.2 s, but it is reasoning, not a measurement.

## 6. PARKED (2026-08-07) — state for whoever picks this up

**Working, uncommitted, held out of the weekday no-commit window.** Everything below is in the tree, both
targets build, and the mechanism is verified on the P4.

Files:
- `components/input/input_exit.c` (new) — the gesture; wall-clock threshold, armed via `input_exit_enable()`
- `components/input/input.h` — declarations
- `components/input/CMakeLists.txt` — compiles `input_exit.c` with **every** backend; `REQUIRES esp_timer`
- `components/input/input_{serial,touch,scheduled}.c` — one `input_exit_check(held)` call each, before returning
- `firmware/pico-e32-fake08/main/main.cpp` — `input_exit_enable(true)` **above** the five-way loop ladder
- `docs/runtime/pico-e32-fake08-input.md` — the `IN-6` spec

**To finish it:**
1. Commit + PR (code change → branch, not `main`) and run a review pass.
2. **The one test the bench cannot do:** flash a **touch** build, run a cart, and hold MENU on the deck for
   ~1.2 s. Confirm it returns to the carousel, and that a *short* tap still pauses the cart instead of exiting.
   Serial cannot stand in for this — see §5.
3. Reflash both boards to shipping builds.

**Board state as of the park:** the **S3 is known-good** — reflashed to the shipping launcher build and confirmed
booting (`SD mounted at /sdcard` → `carousel layout`). The **P4 is not**: it dropped off USB (`/dev/ttyACM0` gone,
no Espressif device on the bus) before it could be reflashed, so it still holds the **serial-input test build** from
§5. Re-plug it and flash the shipping build before trusting anything it shows.

**Also parked:** PR #34 (`wc2-case-colour`, 5 commits, MERGEABLE) — asked to merge at 09:24 Pacific, held for the
same window rule. Three review passes run; the last pass's findings are fixed in `a7b7766` but that commit is
itself unreviewed.

**Known-good baseline if anything here is suspect:** `main` at `65c5001` — both boards ran that cleanly.
