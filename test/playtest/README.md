# test/playtest — agent-solved cart play-tests, FPS measurement & video capture

The end goal: point an **agentic AI** at *any* deterministic PICO-8 cart on the fake-08 port and have it, with
**no human involvement**:

1. **Solve** the cart in the simulator — an agent develops an input sequence that plays it through, using
   this harness as its environment.
2. **Replay** that solution on the real device and confirm it plays there too.
3. **Record** the on-device frame rate — min / max / avg, both *achieved* and *headroom*.
4. **Record** the gameplay as video, from both the simulator and the device, for comparison.

The point of the rig is #3: **measure real on-device FPS across full playthroughs of real carts.**

### Two hard requirements

1. **No human in the loop.** The solution is developed by a **spawned solver agent** driving this harness —
   not by a person playing, and not by a fixed hardcoded search algorithm. The intelligence lives in the
   *agent*; the harness is the environment it experiments in.
2. **The solution is a replay-able `Trace`.** Not a savestate or a solver-internal object — a portable,
   per-game-frame sequence of held buttons ([`trace.py`](trace.py)) that **replays to the intended outcome on
   BOTH the sim and the device**, from the same file. A solve is only accepted once its trace clears on both.

---

## Architecture — the harness is a *gym*, the agent is the solver

The core is genre-agnostic: **trace → replay on sim+device → fps → video.** What varies per cart is only how
the trace is produced, and that is done by an **agent**, not a fixed algorithm. So we do **not** build a
universal solver — we build a universal, fully-observable, deterministic *environment* and let an agent play
in it. Critically:

- **The agent observes the screen.** It looks at rendered frames (like a player), not a Celeste-specific
  state read — that is what generalizes across genres.
- **The agent writes any cart-specific instrumentation itself.** Being a coding agent, it reads the cart's
  `.p8` Lua, finds the win condition / player variables, and writes a targeted state-reader for *that* cart
  (the way Celeste's `READ_LUA` was hand-written). We ship no per-cart adapters; the agent generates them.
- **Algorithmic solvers are optional tools, not the path.** `search.py` is a cart-agnostic replay-from-root
  beam *engine* (a library); a cart wires it up with its own callables + macros (see `celeste/solve.py`).
  The agent may invoke it for a frame-precise sub-problem — everything does not flow through it.

```
test/playtest/
├─ README.md          this document
├─ trace.py           the solution CONTRACT: per-game-frame button masks in ordered segments; the
│                     replay-able artifact both sides consume. Cart-agnostic (opaque per-segment `meta`).
├─ harness.py         generic DEVICE driver: frame-synced input delivery + verification over serial,
│                     clocked by the firmware's telemetry frame counter. Cart-agnostic.
├─ gym.py             the agent's EYES: snapshot() + run_filmstrip() -> viewable PNGs of a run. Cart-agnostic.
├─ render.py          render a Trace playing on the sim -> mp4 + replay() (cart-agnostic; device video = tools/record_video.sh).
├─ orchestrate.py     the ONE-CALL report core (cart-agnostic): sim replay+render + device replay+fps+record -> report.json.
├─ live.py            CLOSED-LOOP runners (cart-agnostic): a `policy(state)->mask` on the sim (drive_sim), LIVE
│                     over serial (drive_device), or TWIN-in-the-loop frame-exact (drive_device_predictive).
├─ fc_sched.py        the fc-SCHEDULED command protocol (encode_cmd) + DeviceScheduler — frame-exact input for the
│                     TWIN shape (the Python twin of firmware components/input/input_scheduled.c). Cart-agnostic.
├─ fake08-sim/        the SHARED native VM — the exact device VM (same components/fake08 + z8lua), headless.
│  ├─ sim.cpp         The agent's GYM: run the real cart, step inputs, read RAM / run Lua, capture frames.
│  ├─ host_sim.cpp    C API + headless Host (scripted input, captured framebuffer, no display/audio).
│  ├─ fake08sim.py    ctypes binding: init / spawn / step / step_mask / steps / read / frame_count / draw / frame_rgb / exec / peek.
│  ├─ Makefile        native g++ build -> libfake08sim.so  (EXTRA_CXXFLAGS/LDFLAGS hook for sanitizers).
│  └─ README.md
├─ search.py          OPTIONAL tool (a LIBRARY): the cart-agnostic replay-from-root beam engine — takes a
│                     cart's callables (reset/observe/win/dead/score/signature) + macros. NOT the generic path.
├─ celeste/           per-cart area (OPEN-LOOP Trace + closed-loop TWIN — a frame-precise cart) — ISOLATED.
│  ├─ solve.py              Celeste's solver: the callables + macros for the shared search.py; emits a Trace.
│  ├─ celeste_playtest.py   OPEN-LOOP device driver: replays the embedded solution, or --trace=<file>.
│  ├─ closed_loop.py        CLOSED-LOOP reactive policies (Climber = 100 M, Climber200 = 200 M) + Chain + obs tail.
│  ├─ fc_device.py          TWIN device driver: room 0,0 closed-loop on the board via fc-scheduled input.
│  ├─ fc_latency.py         the latency-wall + fc-scheduled jitter sweep (why LIVE fails, why TWIN works).
│  ├─ celeste_solver/       the Celeste physics twin + beam search (produced the reference plans).
│  ├─ render_run.py         thin adapter: render Celeste's solution Trace to mp4 (via ../render.py).
│  └─ orchestrate_run.py    thin adapter: one-call sim+device report for Celeste (via ../orchestrate.py).
└─ pico_racer/        per-cart area (continuous racer — closed-loop LIVE + TWIN, M9).
   ├─ solve.py              runs the controller on the sim + emits an open-loop Trace (the sim solution).
   ├─ racer_playtest.py     LIVE: the `policy(state)->mask` + sim/device state reader; drives via ../live.py.
   ├─ racer_fc_device.py    TWIN: the same policy via drive_device_predictive (recovers a big miss on-board).
   ├─ verify_solution.py    fresh-process open-loop sim replay + filmstrip.
   └─ solution.trace.json   the recorded Trace (open-loop; replays on the sim, closed-loop on the device).
```

**Cart-agnostic core** (`trace.py`, `harness.py`, `fake08-sim/`) + **agent-generated per-cart work** (a
state-reader, a plan, verification — whatever the solver agent produces under `<cart>/`).

### The two replayers (both consume a `Trace`)

| side | replayer | reach a segment start | detect the outcome |
|---|---|---|---|
| **sim** | `spawn` + `step_mask()` per frame (`fake08sim.py`) | `sim_start_room` (begin_game + load_room) | the agent's instrumentation / the frame |
| **device** | `harness.run()` delivering frames frame-synced (`celeste_playtest.py --trace`) | telemetry shows the start state | telemetry shows the outcome |

The VM is deterministic and the sim *is* the device's VM, so a trace that clears on the sim clears on the
device (same code + same inputs → same result). Confirmed on Celeste (100 M → 200 M → 300 M).

### Three playtest shapes — open-loop, closed-loop LIVE, or closed-loop TWIN

A cart's playtest can take **any of three shapes**, whichever fits the cart — and **all three run on the sim AND
the device, and all three report on-device fps + record video where possible** (the fps is the cart's real
`Step`+`draw` compute — see [The pipeline](#the-pipeline-per-cart) — so it is the SAME number a human playing
would see, independent of how the input is delivered; video = sim mp4 via `render.py` + device mp4 via
`tools/record_video.sh`):

| shape | what it is | sim runner | device runner | best for |
|---|---|---|---|---|
| **open-loop Trace** | a fixed per-frame mask list (`trace.py`) | `render.replay` / `render.render` | `harness.run` (frame-synced) | SHORT / DISCRETE / self-checking — a Celeste room |
| **closed-loop LIVE** | `policy(state)->mask` — reads the live state each frame and reacts | `live.drive_sim` | `live.drive_device` — reads telemetry off serial, sends buttons **live** | CONTINUOUS control — racing (jitter-tolerant) |
| **closed-loop TWIN** | the SAME `policy` on a lockstep **twin**, delivered FRAME-EXACT + rebased on drift | `live.drive_sim` | `live.drive_device_predictive` — the **fc-scheduled** input backend | FRAME-PRECISE — Celeste dashes (LIVE fails here) |

**open-loop** replays byte-exact, but a long continuous run drifts on hardware (no feedback → any boot/fps/`rnd`
divergence compounds until it fails). **closed-loop LIVE** closes the loop over serial — reads `px`/`speed`/… and
corrects every frame, absorbing that drift — great for CONTINUOUS control but **fatal for frame-precise carts**:
the ~1-frame serial JITTER launches a pixel-exact Celeste dash from the wrong frame → death. **closed-loop TWIN**
is the fix: the policy solves on a deterministic TWIN of the cart (the sim VM), and its per-frame decisions are
delivered **frame-exact** by the fc-scheduled input backend (`INPUT_BACKEND=scheduled` — each command carries a
target frame the device applies it on, so transport jitter can't move the apply instant); the twin is kept in
sync with the board and **rebased** on any divergence. That is what makes closed-loop clear Celeste on the board
(`celeste/fc_device.py`), where LIVE can't. See `docs/runtime/pico-e32-fake08-input.md` (IN-5) + the
[worklog](../../docs/worklog/2026-07-20-celeste-closed-loop-fc-scheduled.md).

**Right strategy per genre** (both proven on the board): FRAME-PRECISE carts use TWIN and **avoid** misses
(recovery from a missed pixel-dash is futile — feedback can't rescue a frame already gone); CONTINUOUS carts use
LIVE (simplest) or TWIN (whose rebase recovers even a big miss — a car knocked off-line steers back). One driver
serves both; only the *reliance* differs. See `celeste/` (TWIN: `closed_loop.py` + `fc_device.py`) and
`pico_racer/` (LIVE: `racer_playtest.py`; TWIN: `racer_fc_device.py`).

Tip: `live.drive_sim(..., record=True)` also RECORDS the masks a policy emits — so a closed-loop controller can
*also* produce an open-loop Trace when a cart wants both (that is what a `solve.py` does).

### The device is cart-agnostic — the host tells it *what* to stream

The firmware carries **no per-cart telemetry**. Each frame it emits `T <fc> <step_us> <draw_us> <tail>`: the
prefix is generic (frame clock + compute, for sync + fps); the **tail** is a Lua expression, read out of the
cart's globals via `ExecuteLua`, that the cart's driver **sends over serial at startup** — not compiled in. The
handshake: the board prompts `CFG?`, the driver replies `TT<expr>`. So a new cart is drop-in: its driver
defines both `parse_line` (how to read the tail) and the tail expression (what to stream), and nothing in the
firmware changes. The runners send it for you — `harness.run(telemetry_config=…)` /
`live.drive_device(telemetry_config=…)`; see `pico_racer/racer_playtest.py`'s `RACER_TAIL`. Built with
`-D TELEMETRY=1 -D TELEMETRY_HOST_CFG=1`; the whole host-command path is **dev/HITL-only and compiles out of
production** (which defines no `TELEMETRY`). A plain `-D TELEMETRY=1` build (no HOST_CFG) keeps the inline
Celeste tail as a convenience default — Celeste's frame-precise timing then uses the exact original code path.

**Faster / lower-jitter telemetry (optional, for latency-sensitive control).** Two further dev-only flags cut
the serial cost of streaming state every frame: `-D TELEMETRY_BAUD=921600` runtime-switches the console UART up
after the CFG handshake (less serialization *jitter*), and `-D TELEMETRY_BINARY=1 -D TELEMETRY_BINARY_BYTES=N`
makes the tail a `poke` that writes the observation as int16s into scratch RAM, which the firmware then ships
**raw + sync-framed** (`0xAA 0x55` + fc/step/draw + N bytes) instead of an ASCII line — no `fix32`→string / GC
per frame. The driver reads it via `live.drive_device(binary=(sync, packet_len, unpack))`, where `unpack` stays
cart-specific (the binary twin of `parse_line`). The racer uses both by default; on that cart the **baud** was
the real win (control jitter), while **binary** was within noise — cheaper on the wire, but the bottleneck was
latency, not bytes. See `pico_racer/racer_playtest.py` (`RACER_TAIL_BIN`, `unpack_binary`) and the M9 worklog.

---

## How solving works — a spawned solver agent + this gym

For each cart, a **solver agent** is spawned and given this harness as its toolset. Its loop:

```
  inspect the cart (read its .p8 Lua; boot it in the sim; LOOK at rendered frames)  →  understand it
        ↓
  hypothesize inputs  →  run them in the sim  →  OBSERVE (frames / filmstrip + any state it instrumented)
        ↓                                              ↑
  accumulate a Trace  ←  iterate / branch (replay-from-root; invoke search for frame-precise bits)
        ↓
  VERIFY the full Trace replays to the intended outcome on the sim   →  hand off to the device pipeline
```

The agent supplies the per-cart intelligence — what the cart is, what "played through" means (a win screen, a
score, credits, a level count — it decides by looking), and how to instrument state (by reading the Lua). The
harness supplies the deterministic, observable environment and the trace contract. Spawn one agent per cart to
scale across many carts.

For a **continuous-control** cart the loop ends differently: instead of accumulating a fixed Trace, the agent
tunes a **`policy(state) -> mask`** and hands off a *live* controller (see [Three playtest shapes](#three-playtest-shapes--open-loop-closed-loop-live-or-closed-loop-twin)). It still develops + verifies on the sim (`live.drive_sim`, which can also record a Trace); the device runs the same policy live (`live.drive_device`).

### Standalone & isolated — the per-cart contract

A solver agent is free to **use tools and write whatever scripts it needs** to crack a cart — but its entire
output must be **standalone and isolated in that cart's directory**, `test/playtest/<cart>/`:

- **Everything the agent creates lives under `<cart>/`** — its solver scripts, any cart-specific
  instrumentation (e.g. a state-reader it wrote after reading the cart's Lua), notes, and the solution trace.
- **The shared core is read-only.** The agent *imports* the gym and contract as libraries
  (`fake08-sim/fake08sim.py`, `trace.py`, `harness.py`, `live.py`, optional `search.py`) but must not modify
  them or any other cart. A new shared capability is a framework change decided outside the solve — not
  something a solver agent does ad hoc.
- **The result is a self-contained solution package.** Dropping `<cart>/` gives the full recipe for that cart
  and nothing else: reproducible (run its scripts), auditable (read them), and unable to affect other carts or
  the framework.

Conventional layout (the orchestrator looks for these):

```
test/playtest/<cart>/
├─ solve.py             the agent's standalone solver — run it to (re)produce the trace
├─ solution.trace.json  the verified replay-able Trace (the deliverable)
├─ <helpers…>           any scripts / instrumentation the agent wrote (isolated here)
└─ README.md            the agent's notes: what the cart is, how it solved it
```

### The solver-agent toolbox (what the gym exposes)

- **See** (`gym.py`) — `gym.snapshot(path)` renders the current frame to a PNG; `gym.run_filmstrip(masks,
  path, ...)` replays a run and montages sampled, labelled frames into one contact-sheet PNG the agent Reads
  in a single look (motion/progress, not just a still). Raw `frame_rgb()` / `draw()` underneath.
- **Act** — `step(keys)` / `step_mask(mask)` / `steps(masks)` (batch, fast, no read).
- **Observe/instrument** — `read()` (Celeste convenience; also returns `fc`, the frame count), `frame_count()`
  (the VM clock == device telemetry frame — sync solutions to it), `sim_exec(lua)` + `sim_peek(addr)` to run
  Lua in the cart sandbox and read RAM — the agent uses these to build its *own* per-cart state reader.
- **Branch/verify** — reset via `spawn` + replay-from-root (savestates are parked); "replay this Trace →
  outcome".
- **Trace** — `trace.py`: accumulate / edit / save / load / verify; the hand-off artifact.
- **Collaborate** — solving is not blind. While it works, the agent may **share the current image** (a
  `gym.snapshot` / `run_filmstrip` PNG) with the owner and **ask a question** — when it's stuck, hits an
  ambiguity, or needs a decision — rather than guessing. It surfaces the image + question through its run so
  the owner can look and answer, then continues.

---

## Key design decisions

- **Intelligence in the agent, not a fixed algorithm.** An LLM agent generalizes across genres far better
  than any single search; the harness gives it eyes (frames), hands (inputs), and instrumentation (Lua/RAM).
- **Observe via the screen.** Rendered frames are the universal observation; per-cart state readers are
  written by the agent, not shipped by us.
- **The trace is the contract.** One artifact, replay-able on both sides; accepted only if it clears on both.
- **Frame count is the shared sync clock.** Both the device telemetry and the host sim expose the VM's
  per-Step counter (`GetFrameCount`, +2 per 30 fps game-frame — `fake08sim.frame_count()`, also in every
  `read()['fc']`; device: `T <frame> …`). A solution built on the host is *indexed by this count*, so replay
  lands each button press at the matching device frame. The counts have different absolute origins (different
  boot/spawn sequences), so the harness **anchors** the host's relative frames to the device's telemetry
  count at each segment start (`plan_fc0`) — the frame count is the clock, the anchor aligns the phase.
- **Replay-from-root, not VM savestates.** Branching by `spawn + replay(prefix)` is robust and its output is
  a replay-able trace by construction. The agent works in reasoned chunks, so it rarely needs O(1) restore —
  which keeps the parked eris savestate work off the critical path.
- **FPS is a device measurement with two notions** — *achieved* (does the device hold the cart's target rate;
  min = worst dip) and *headroom* (`1000/(step+draw)`). Record per-frame timing; report both. The sim runs
  uncapped on a desktop, so **sim video is for visual comparison only — never an fps source.**

---

## The pipeline (per cart)

```
  spawned solver agent  ──►  Trace (per-frame masks)  ──►  ┌── sim:    replay + render sim.mp4
  (uses this gym)             the replay-able artifact       └── device: flash telemetry build, replay
                                                                       frame-synced, record fps + camera device.mp4
                                                            ──►  report { fps min/max/avg (achieved+headroom),
                                                                          sim.mp4, device.mp4, pass/fail }
```
The right-hand side (everything after the Trace) is exactly `orchestrate.py` — one call, one `report.json`.

---

## Milestones

| # | milestone | status |
|---|---|---|
| M0 | Reorg into `test/playtest/` (shared VM + per-cart dirs) | ✅ done |
| M1 | Replay-from-root backend validated (deterministic; reproduces the clear) | ✅ done |
| M2 | Portable `Trace` + dual replay proven — same file clears on sim **and** device | ✅ done |
| M3 | Beam search tool — cart-agnostic engine `search.py` + Celeste adapter `celeste/solve.py`; climbs a room on the exact VM | ✅ done (optional tool) |
| M4 | **Agent-facing gym** — the eyes: `gym.snapshot` + `gym.run_filmstrip` render viewable frame/filmstrip PNGs of a run (verified on Celeste room 0) | ✅ done |
| M5 | **Spawned solver-agent flow**: agent read the cart, drove the gym, wrote its own scripts **isolated under `celeste/`**, and emitted a verified `Trace` — **proven on Celeste** (independent sim replay + device replay 2/2 on the board) | ✅ done |
| M6 | Unified fps telemetry (achieved **and** headroom) + harness aggregation → min/avg/max — **on the board** | ✅ done |
| M7 | Generalize video capture — shared `render.py` (Trace → sim mp4, cart-agnostic) + Celeste adapter; device via `record_video.sh` | ✅ done |
| M8 | One-call orchestrator — shared `orchestrate.py` + Celeste `orchestrate_run.py`: sim replay+render **and** device replay+fps+camera → one `report.json` (+ sim.mp4/device.mp4) — **verified on the board** | ✅ done |
| M9 | Second cart (non-platformer) — a spawned agent solved **Pico Racer** (a score-max racer, `rnd()`-driven) on the gym with NO framework changes → verified Trace (tpos 63/141); drives on the board (closed-loop LIVE, `pico_racer/`) | ✅ done |
| M10 | **Third playtest shape: closed-loop TWIN** — frame-exact input on the board via the **fc-scheduled** backend (`components/input/input_scheduled.c`) + `live.drive_device_predictive`. Makes closed-loop clear **Celeste on the board** (deterministic), where LIVE is jitter-fatal; the same driver drives the racer + recovers a big miss. Right strategy per genre (TWIN avoids misses for frame-precise; LIVE/rebase for continuous) — both proven on hardware. The driver skips the title itself over the same backend (jump commands, no `CELESTE_START` autostart) and plays room-to-room as a **chain** (100 M → 200 M cleared closed-loop on-board, 260 commands, 0 divergences, fps 9.6/29.8/30.0 — = the open-loop number; fps is input-shape-independent). See [worklog](../../docs/worklog/2026-07-20-celeste-closed-loop-fc-scheduled.md) + [IN-5](../../docs/runtime/pico-e32-fake08-input.md) | ✅ done |
| — | **Parked:** eris VM savestates (O(1) restore) — diagnosed, ASAN harness in place; off the critical path (would make the TWIN rebase O(1) instead of replay-from-root) | 🅿️ parked |

---

## TODO / backlog (detail)

- **M4 — agent-facing gym. ✅ done.** `gym.py`: `snapshot(path)` renders the current frame, and
  `run_filmstrip(masks, path, every=, reset=, label=)` replays a run and montages sampled, labelled frames
  into one contact-sheet PNG (returns the captured `read()` states too) — so a solver agent experiments and
  *observes* in one look. Deterministic. Verified on Celeste room 0 (the climb reads y96 → y4 across the strip).
- **M5 — solver-agent flow. ✅ done.** A spawned solver agent, given the gym as its toolset and scoped to
  `test/playtest/celeste/`, inspected the cart with `gym.snapshot`, derived each room's input by re-running
  the twin's beam search and **VM-gating every route** (a real finding: several twin "wins" fail on the exact
  VM — the twin is a model, the VM is ground truth), and emitted `celeste/solution.trace.json` (2 segments,
  207 frames) plus `make_solution.py` / `verify_solution.py` / `README.md` — all isolated in `celeste/`, no
  shared code touched. Independently verified: sim replay clears both rooms, a filmstrip eye-check, and
  **device replay 2/2 on the board**. Solver agents may also **share images + ask the owner questions** while
  solving (the Collaborate affordance).
- **M6 — fps. ✅ done.** The `TELEMETRY` loop (`firmware/pico-e32-fake08/main/main.cpp`) now times
  `vm->Step()` + `host->drawFrame()` and streams `T <fc> <step_us> <draw_us> <cart-state…>` (the ExecuteLua
  poke is not timed, so it's the cart's real compute). `harness.FpsMeter` groups the per-Step timing into
  game-frames and reports min/avg/max **achieved** (`min(target, ceiling)` — does it hold the rate?) and
  **headroom** (`1e6/compute` — the uncapped ceiling); `harness.run(fps_out=…)` returns the stats. Measured on
  the board while clearing Celeste: achieved 9.2/29.9/30.0, headroom 9.2/64.1/112.6 over 510 game-frames.
  (Fold into the trace `meta` + a report at M8.)
- **M7 — video. ✅ done.** `render.py` (shared, cart-agnostic) plays a `Trace` on the sim → mp4, driven by
  cart callables (`reset(seg)`, `stop_on_clear(state, seg)`); `celeste/render_run.py` is now a thin adapter
  (`spawn(rx,ry)` + room-advance). Device video already works via
  [`tools/record_video.sh`](../../tools/record_video.sh) (`--` a `celeste_playtest.py --trace=<file>`; SVGA,
  see the bench-camera doc) — cart-agnostic (records whatever plays). Verified: rendered the M5
  `solution.trace.json` (257 frames / 8.6 s) and filmed the same trace on the board.
- **M8 — orchestrator. ✅ done.** `orchestrate.py` (shared, cart-agnostic) folds a solved `Trace` through
  both sides in one call: sim `replay()` (verify) + `render()` (sim.mp4), then the device replay (verify +
  fps, via a cart hook) + a bench-camera video (`record_video.sh`), and writes `report.json`
  `{cart, segments, frames, sim{cleared,total,video,per-segment}, device{cleared,total,fps{achieved,headroom},video}, pass}`.
  A cart supplies only its sim `reset`/`stop` callables + device replay/record hooks; `celeste/orchestrate_run.py`
  is the thin Celeste entry (`--sim-only` to skip the board). Verified on the board: sim 2/2 + device 2/2,
  achieved 9.2/29.9/30.0, headroom 9.2/64.0/112.1 over 507 game-frames, sim.mp4 (8.6 s) + device.mp4 (22.6 s).
- **M9 — 2nd cart. ✅ done.** A spawned solver agent solved **Pico Racer** (`assets/Pico Racer.p8.png`, a
  pseudo-3D score-max racer by kometbomb) on the gym with **zero framework changes** — proving the approach
  isn't Celeste-shaped. It's a genuinely different genre: continuous control (not a puzzle-clear), objective =
  max distance (`tpos`) before a `clock` times out (not a room advance), and `rnd()`-driven traffic. The
  agent wrote its own state reader + controller under `pico_racer/`, pinned `rnd()` with `srand(seed)` for
  determinism, and emitted a verified Trace (**tpos 63/141**, 0 crashes; open-loop replay reproduces it).
  Two findings surfaced that fed back into the shared stack: (1) `.p8.png` carts load with no code (fake-08
  auto-decodes PNG — see M8-era notes); (2) **open-loop trace replay is fragile for long continuous-control
  runs** on hardware (errors compound with no feedback) — so the racer's DEVICE driver (`racer_playtest.py`)
  is **closed-loop** (read `px`/`speed` telemetry, steer live), which drives the car robustly on the board
  (filmed). Firmware gained a cart-agnostic `RACER` telemetry tail + an `RND_SEED` PRNG pin.

---

## How to run

**Identify the board** (ports are not stable): the device is the **CP2104**, the bench camera is FTDI.
```sh
for d in /dev/ttyUSB*; do echo "$d $(udevadm info -q property -n $d | grep -m1 ID_USB_DRIVER=)"; done
# cp210x -> the board (use this)   ;   ftdi_sio -> the camera
```

**Flash the firmware** (telemetry + frame-exact input) once:
```sh
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<board> \
     DEFS='-D CELESTE=1 -D INPUT_BACKEND=serial -D INPUT_HOLD_FRAMES=1 \
           -D FORCE_FLASH_CART=1 -D SHOW_FPS=1 -D TELEMETRY=1 -D CENTER_GAME=1'
```
See [`docs/runtime/pico-e32-fake08-input.md`](../../docs/runtime/pico-e32-fake08-input.md).

**Build the shared sim (the gym):**
```sh
make -C test/playtest/fake08-sim            # -> libfake08sim.so
```

**Host-side tests** (fast, deterministic — run standalone with exit 0/1, or under pytest):
```sh
python3 test/playtest/test_trace.py         # Trace mask<->keys round-trip + save/load + version guard (pure Python)
python3 test/playtest/test_fps.py           # FpsMeter grouping + achieved/headroom stats (pure Python)
python3 test/playtest/test_sim_smoke.py     # VM builds+boots Celeste, room-0 plan clears, frame count +2/game-frame
python3 test/playtest/test_orchestrate.py   # orchestrate() sim path: both rooms clear, report.json well-formed, sim.mp4 written
```
`test_sim_smoke.py` / `test_orchestrate.py` need the sim built (they SKIP cleanly otherwise).

**Replay a solved trace** — the *same file* on both sides (each prints `CLEARED …` per segment then
`PASS`/`FAIL`; the device run also prints the fps summary):
```sh
# device: deliver the trace frame-synced over serial (identify <board> = CP2104) and measure fps
python3 test/playtest/celeste/celeste_playtest.py <board> --trace=test/playtest/celeste/solution.trace.json

# sim: replay the same trace on the exact VM (fast, no hardware)
python3 test/playtest/celeste/celeste_playtest.py --sim --trace=test/playtest/celeste/solution.trace.json
```

**Run a continuous-control cart (Pico Racer) — closed-loop live, or open-loop replay.** The racer's primary
solution is a live **closed-loop policy** (read telemetry, steer each frame); the same `policy(state)->mask`
drives the sim and the board. The open-loop trace path is kept for comparison (fragile on hardware — see
[Three playtest shapes](#three-playtest-shapes--open-loop-closed-loop-live-or-closed-loop-twin)).
```sh
# CLOSED-LOOP (default, robust) — the SAME policy on each target:
python3 test/playtest/pico_racer/racer_playtest.py <board>   # device: read telemetry + steer live (dodges traffic)
python3 test/playtest/pico_racer/racer_playtest.py --sim      # sim: identical policy, no board needed

# OPEN-LOOP trace replay (frame-synced; fragile on hardware, for comparison):
python3 test/playtest/pico_racer/racer_playtest.py <board> --replay
```
The device path needs the racer firmware (binary telemetry + higher baud are the DEFAULT — they tighten the
latency-sensitive dodge; see the racer README + M9 worklog):
```sh
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<board> \
  DEFS='-D FORCE_FLASH_CART=1 -D RACER=1 -D TELEMETRY=1 -D TELEMETRY_HOST_CFG=1 \
        -D TELEMETRY_BAUD=921600 -D TELEMETRY_BINARY=1 -D TELEMETRY_BINARY_BYTES=40 -D RND_SEED=39 \
        -D INPUT_BACKEND=serial -D INPUT_HOLD_FRAMES=2 -D SHOW_FPS=1 -D CENTER_GAME=1'
```

**Run a frame-precise cart closed-loop on the board — the TWIN shape (fc-scheduled).** LIVE closed-loop is
jitter-fatal for a pixel-exact dash, so the policy solves on a lockstep twin and is delivered FRAME-EXACT via the
fc-scheduled input backend. Celeste clears on the board, deterministically, as a CHAIN — 100 M → 200 M → 300 M
(the board reaches room N only by clearing N-1, so the whole run is played through). Two delivery shapes over the
same backend: **twin-in-the-loop** (rebase on divergence) clears 100 M/200 M; **open-loop** (blind fc-scheduled
replay) clears all three including the frame-precise 300 M — the rebase is counterproductive there (it re-plans on
a dash's transient divergence and can't recover a missed pixel-dash), so blind replay on the deterministic board
wins:
```sh
# firmware: the fc-scheduled input backend + Celeste (the driver skips the title itself, via jump commands)
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<board> \
  DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1 -D INPUT_BACKEND=scheduled \
        -D TELEMETRY=1 -D SHOW_FPS=1 -D CENTER_GAME=1'
python3 test/playtest/celeste/fc_device.py <board> --openloop --to300  # 100→200→300 OPEN-LOOP (clears 300 M, deterministic)
python3 test/playtest/celeste/fc_device.py <board> --predictive        # twin-in-the-loop, 100 M -> 200 M, rebase on divergence
python3 test/playtest/celeste/fc_device.py <board> --predictive --room0 # just 100 M
python3 test/playtest/celeste/render_compare.py 300m                    # sim | device (open-loop) side-by-side video
python3 test/playtest/pico_racer/racer_fc_device.py <board>            # the racer through the SAME predictive driver
```

**Solve a cart** — a spawned solver agent, given the gym + the cart, produces a verified
`test/playtest/<cart>/solution.trace.json` (done for Celeste — see `celeste/README.md`).

**Render / record video** of a trace:
```sh
python3 test/playtest/celeste/render_run.py sim_run.mp4 [trace.json]                              # sim -> mp4
tools/record_video.sh -o device.mp4 -- python3 test/playtest/celeste/celeste_playtest.py <board> --trace=…  # device
```

**One-call orchestrator** — run the whole play-test (sim replay+render **and** device replay+fps+camera) and
fold it into one `report.json` (+ `sim.mp4`, `device.mp4`) under `--out` (default `/tmp/celeste-playtest`):
```sh
python3 test/playtest/celeste/orchestrate_run.py <board> [--trace=<file>] [--out=<dir>]   # sim + device + report
python3 test/playtest/celeste/orchestrate_run.py --sim-only                               # sim half only (no board)
```

---

## Parked: eris VM savestates

O(1) snapshot/restore would speed a *search* tool, but the vendored eris path is disabled and buggy — fully
diagnosed (2026-07-19): enabling `eris.init_persist_all()`
(`components/fake08/fake08/source/vm.cpp:170-184`) makes a single round-trip bit-exact, but under load it
hits multiple memory bugs (use-after-free in `u_proto` on unpersist; global-buffer-overflow in `Vm::Step`
after restore) — which is why upstream commit `47c48ad` disabled it. Off the critical path: the agent
branches via replay-from-root, and works in reasoned chunks rather than brute-force wide beams. Left for a
future pass: the round-trip acceptance test and an ASAN build hook in `fake08-sim/Makefile`
(`make clean && make EXTRA_CXXFLAGS='-fsanitize=address -g -O0' EXTRA_LDFLAGS='-fsanitize=address'`, run
under `LD_PRELOAD=$(gcc -print-file-name=libasan.so)`).

---

## References

- [`fake08-sim/README.md`](fake08-sim/README.md) — the shared VM internals + C API.
- [`celeste/celeste_solver/README.md`](celeste/celeste_solver/README.md) — the physics twin + beam search.
- [`docs/runtime/pico-e32-fake08-input.md`](../../docs/runtime/pico-e32-fake08-input.md) — serial input backend + telemetry build.
- [`docs/hardware/pico-e32-bench-camera.md`](../../docs/hardware/pico-e32-bench-camera.md) — the device video path.
- [`docs/worklog/2026-07-18-celeste-playtest-clear.md`](../../docs/worklog/2026-07-18-celeste-playtest-clear.md) — the reference Celeste solution.
