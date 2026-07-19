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
- **Algorithmic solvers are optional tools, not the path.** `search.py` (a replay-from-root beam) is one
  thing the agent may *invoke* for a frame-precise sub-problem — everything does not flow through it.

```
test/playtest/
├─ README.md          this document
├─ trace.py           the solution CONTRACT: per-game-frame button masks in ordered segments; the
│                     replay-able artifact both sides consume. Cart-agnostic (opaque per-segment `meta`).
├─ harness.py         generic DEVICE driver: frame-synced input delivery + verification over serial,
│                     clocked by the firmware's telemetry frame counter. Cart-agnostic.
├─ fake08-sim/        the SHARED native VM — the exact device VM (same components/fake08 + z8lua), headless.
│  ├─ sim.cpp         The agent's GYM: run the real cart, step inputs, read RAM / run Lua, capture frames.
│  ├─ host_sim.cpp    C API + headless Host (scripted input, captured framebuffer, no display/audio).
│  ├─ fake08sim.py    ctypes binding: init / spawn / step / step_mask / steps / read / frame_rgb / exec / peek.
│  ├─ Makefile        native g++ build -> libfake08sim.so  (EXTRA_CXXFLAGS/LDFLAGS hook for sanitizers).
│  └─ README.md
├─ search.py          OPTIONAL tool: replay-from-root beam search (a platformer-traversal solver the agent
│                     may invoke for precise sub-problems). NOT the generic path.
└─ celeste/           per-cart area — a solver agent's ISOLATED workspace + standalone solution package
   │                  (Celeste is the first proof cart). Shared core imported read-only; nothing leaks out.
   ├─ celeste_playtest.py   DEVICE driver: replays the embedded solution, or --trace=<file>.
   ├─ celeste_solver/       the Celeste physics twin + beam search (produced the reference plans).
   └─ render_run.py         render a pixel-perfect sim run to mp4.
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

### Standalone & isolated — the per-cart contract

A solver agent is free to **use tools and write whatever scripts it needs** to crack a cart — but its entire
output must be **standalone and isolated in that cart's directory**, `test/playtest/<cart>/`:

- **Everything the agent creates lives under `<cart>/`** — its solver scripts, any cart-specific
  instrumentation (e.g. a state-reader it wrote after reading the cart's Lua), notes, and the solution trace.
- **The shared core is read-only.** The agent *imports* the gym and contract as libraries
  (`fake08-sim/fake08sim.py`, `trace.py`, `harness.py`, optional `search.py`) but must not modify them or any
  other cart. A new shared capability is a framework change decided outside the solve — not something a solver
  agent does ad hoc.
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

- **See** — `frame_rgb()` → the current 128×128 frame; **(planned)** render a candidate run to a *filmstrip*
  image (a grid of sampled frames) so the agent perceives motion in one look.
- **Act** — `step(keys)` / `step_mask(mask)` / `steps(masks)` (batch, fast, no read).
- **Observe/instrument** — `read()` (Celeste convenience; also returns `fc`, the frame count), `frame_count()`
  (the VM clock == device telemetry frame — sync solutions to it), `sim_exec(lua)` + `sim_peek(addr)` to run
  Lua in the cart sandbox and read RAM — the agent uses these to build its *own* per-cart state reader.
- **Branch/verify** — reset via `spawn` + replay-from-root (savestates are parked); "replay this Trace →
  outcome".
- **Trace** — `trace.py`: accumulate / edit / save / load / verify; the hand-off artifact.

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

---

## Milestones

| # | milestone | status |
|---|---|---|
| M0 | Reorg into `test/playtest/` (shared VM + per-cart dirs) | ✅ done |
| M1 | Replay-from-root backend validated (deterministic; reproduces the clear) | ✅ done |
| M2 | Portable `Trace` + dual replay proven — same file clears on sim **and** device | ✅ done |
| M3 | Beam search tool (`search.py`) — validated it climbs a room on the exact VM | ✅ done (demoted to optional tool) |
| M4 | **Agent-facing gym**: filmstrip/frame rendering + clean run/branch/verify primitives an agent can drive | ⏳ next |
| M5 | **Spawned solver-agent flow**: agent reads the cart, drives the gym, writes its own scripts/instrumentation **standalone & isolated under `<cart>/`**, emits a verified `Trace` — **proven on Celeste** | ⏳ next |
| M6 | Unified fps telemetry (achieved **and** headroom) + harness aggregation → min/max/avg | 📋 todo |
| M7 | Generalize video capture (sim `render_run` → any cart+trace; device `record_video.sh` already generic) | 📋 todo |
| M8 | One-call orchestrator: `playtest <cart>` → spawn solver → sim + device → emit the report | 📋 todo |
| M9 | Second cart (non-platformer) — proves the agent+gym is genuinely genre-agnostic | 📋 todo |
| — | **Parked:** eris VM savestates (O(1) restore) — diagnosed, ASAN harness in place; off the critical path | 🅿️ parked |

---

## TODO / backlog (detail)

- **M4 — agent-facing gym.** Add a **filmstrip** renderer (`frame_rgb` of sampled frames → a labeled grid
  PNG the agent can view in one look) and a `run(inputs) → {final frame, filmstrip, instrumented state}`
  primitive, so a solver agent can experiment and *observe* efficiently. Keep everything deterministic.
- **M5 — solver-agent flow.** Spawn a per-cart solver agent with the gym as its toolset, scoped to
  `test/playtest/<cart>/` per the **standalone & isolated contract** above: it reads the cart's Lua, writes
  its own scripts/state-reader *there*, iterates to a `Trace`, self-verifies via sim replay, and touches no
  shared code. Prove it on Celeste (compare against the known-good reference trace). May invoke `search.py`
  for frame-precise bits.
- **M6 — fps.** `MEASURE_FPS` and `TELEMETRY` are today mutually-exclusive loops in
  `firmware/pico-e32-fake08/main/main.cpp`. Unify: one loop streaming `T <fc> <step_us> <draw_us> …`. Harness
  records the per-frame series → min/max/avg **achieved** + **headroom**. Fold into the trace `meta` + report.
- **M7 — video.** Generalize `celeste/render_run.py` to `(cart, trace) → sim.mp4`. Device video already works
  via [`tools/record_video.sh`](../../tools/record_video.sh) (SVGA; see the bench-camera doc).
- **M8 — orchestrator.** One entry: spawn the solver → sim replay+render → device flash+replay+fps+camera →
  write the report `{fps stats, sim.mp4, device.mp4, pass/fail}`.
- **M9 — 2nd cart.** A non-platformer cart to validate that the gym + agent approach isn't Celeste-shaped:
  the agent should read the new cart and produce a trace with no framework changes.

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
python3 test/playtest/test_sim_smoke.py     # VM builds+boots Celeste, room-0 plan clears, frame count +2/game-frame
```
`test_sim_smoke.py` needs the sim built (it SKIPs cleanly otherwise).

**Replay a solved trace** — the same file on both sides:
```sh
python3 test/playtest/celeste/celeste_playtest.py <board> --trace=solution.trace.json   # device
# sim: spawn each segment, step its masks, check the outcome (see fake08sim.step_mask / trace.py)
```

**Solve a cart** (spawned solver agent, once M4/M5 land): a per-cart solver agent is given the gym and the
cart, and produces `solution.trace.json`. **Render / record video:**
```sh
python3 test/playtest/celeste/render_run.py sim_run.mp4                                          # sim
tools/record_video.sh -o device.mp4 -- python3 test/playtest/celeste/celeste_playtest.py <board> # device
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
