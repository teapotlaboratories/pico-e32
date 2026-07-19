# fake08-sim — the device VM, headless on the desktop

A native build of the **exact fake-08 VM the device runs** (same `components/fake08` source + the same
`components/z8lua`), driven headlessly for **solving and verifying** a cart's input plans off-device
(Celeste today), and for rendering a **pixel-perfect video** to compare against the bench camera. It runs
the real cart in the real VM, so there is no hand-written physics model to keep in sync — this is the ground
truth the Python twin (`test/playtest/celeste/celeste_solver/celeste_sim.py`) was validated against.

This is the **shared** VM under `test/playtest/`: cart-agnostic. Cart-specific tooling (e.g. Celeste's
`render_run.py`, its solver) lives under the game's own dir (`test/playtest/celeste/`).

Built by linking the vendored fake-08 source **without modifying it**: z8lua `.c` compile as C++ (as the
component does), and `logger.cpp` + `main.cpp` are replaced by our stubs (exactly as the vendored fake-08
*test* build does). The Host is our own headless implementation (`host_sim.cpp`) — scripted input, a captured
framebuffer, no display/audio — so the VM can be single-stepped.

## Files

| file | what |
|------|------|
| `host_sim.cpp` | Headless `Host`: `scanInput()` returns scripted input; `drawFrame()` captures the 128×128 framebuffer; timing/audio are no-ops. |
| `sim.cpp` | C API (below), incl. reading the player via `ExecuteLua` poking scratch RAM (`0x4300`), and rendering RGB frames. |
| `Makefile` | Native `g++` build → `libfake08sim.so`. |
| `fake08sim.py` | ctypes bindings + `spawn()` / `replay()` helpers. |

(Celeste's `render_run.py` — render the two solved rooms to an mp4 — is cart-specific and lives in
`../celeste/render_run.py`.)

## Build + use

```sh
make -C test/playtest/fake08-sim              # -> libfake08sim.so
python3 -c "import sys; sys.path.insert(0,'test/playtest/fake08-sim'); import fake08sim as S; \
  S.init('assets/celeste.p8'); print(S.spawn(0,0))"     # -> {x:8, y:96, rx:0, ry:0, dj:1}  (matches the device)
python3 test/playtest/celeste/render_run.py sim_run.mp4  # pixel-perfect run video (Celeste)
```

C API: `sim_init(cart)` · `sim_start_room(rx,ry)` / `spawn()` · `sim_step(mask)` · `sim_read(...)` ·
`sim_draw()` + `sim_frame_rgb(out)` · `sim_save()`/`sim_restore()` · `sim_save_buf`/`sim_restore_buf`.

## The intended flow (solve → verify → compare)

1. **Solve** a room's inputs against the exact VM.
2. **Verify** the plan on the VM (`fake08sim.replay(plan)` → room advances) and render `sim_run.mp4`.
3. **Deliver** the plan to the device (`test/playtest/celeste/celeste_playtest.py`, frame-synced serial) and verify via
   telemetry + camera.
4. **Compare** the emulator video and the camera video (see the worklog for a side-by-side).

Validated: both shipped plans (`PLAN_100M`, `PLAN_200M`) clear on this exact VM (`replay()` → room advances),
and spawn / standing-jump apex / dash all match the device to the pixel.

## Caveat — the search currently runs on the twin, validated here

Moving the **beam search** onto this VM efficiently needs per-node savestates. fake-08 has
`Vm::serializeLuaState` (eris), but it returns 0 here: the VM's `init_persist_all` (which populates the eris
`perm` table) is commented out, and the main Lua state isn't reachable from the sandbox `ExecuteLua` to set it
up — enabling it would need a small vendored getter. So today the search runs on the fast Python twin
(`test/playtest/celeste/celeste_solver`), which **this sim has proven exact**; the sim is the validator +
renderer. Making savestates work (for a fully VM-native search) is the open follow-up.
