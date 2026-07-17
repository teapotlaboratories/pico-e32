# 2026-07-17 — chasing the fake-08 room-transition spike (it's compute, not GC)

The [gameplay fps measurement](2026-07-17-fake08-fps-strip-blit.md) found that steady Celeste runs ~110 fps
on the S3, but every room load/transition costs a **single ~95–100 ms frame** (~10 fps for that frame).
This chases *what* costs the 95 ms — and rules out the obvious suspect.

## Method

Extended the opt-in `MEASURE_FPS` build (`firmware/pico-e32-fake08/main/main.cpp`) to sample the **system
heap** each frame (z8lua's default allocator is `realloc`, so the sys heap reflects Lua alloc/free) and
**isolate spike frames** (log any frame whose `Step()` > 30 ms with its heap-before→after delta). Drove
Celeste through rooms with a **temporary** `scanInput` auto-drive (walk right / jump / climb; reverted).

## What the spike frames showed

`drawFrame` stays a flat **3.6 ms** on spikes, so the ~95 ms is entirely `Step()` — the Lua VM. And the
spike frames move the heap a lot:

| frame | step | heap Δ | reading |
|---|---|---|---|
| f0 | 246 ms | −289 KB | boot / `_init` (one-time) |
| f175 | 104 ms | **+12 KB** (net freed) | a GC-dominant frame |
| f387 | 95 ms | **−54 KB** (allocated) | `load_room` — spawns a room's objects |
| f611 | 95 ms | **−54 KB** (allocated) | `load_room` — spawns a room's objects |

So a room-load frame is Celeste's `load_room` scanning the 16×16 map and **allocating/initializing ~54 KB
of objects** on the interpreter. Open question: is the 95 ms the `load_room` *logic*, or **z8lua's
incremental GC**, which does collection work *proportional* to that 54 KB burst (default `GCMUL=200` →
~2× = ~108 KB scanned, concentrated in the same frame)?

## The decisive experiment — make GC lazy, re-measure

Tuned our z8lua fork's GC defaults (`lstate.c`, no fake-08 `source/` touched): **`GCPAUSE 200→400`,
`GCMUL 200→80`** (start cycles later; collect far less per allocation). Re-measured the same spikes:

| spike | baseline | lazy GC | verdict |
|---|---|---|---|
| f175 (GC-dominant, +12 KB) | 104 ms | **77 ms** | GC-bound → helped |
| f387 (`load_room`, −54 KB) | 95 ms | **96 ms** | **unchanged** |
| f611 (`load_room`, −54 KB) | 95 ms | **96 ms** | **unchanged** |

**Conclusion: the room-load spikes are inherent interpreter compute, NOT GC.** Halving the GC rate left
them untouched; it only sped a GC-dominant frame. So GC tuning does **not** fix the transition hitch.

## Implication

There is **no cheap separate fix** for the transition spike — it's the same z8lua-interpreter-speed limit
(the [z8lua-speedup](../reference/z8lua-speedup-research.md) item), concentrated on the heaviest
(room-load) frame. Reducing it would need a generally faster VM (which also helps steady gameplay), or a
cart change (not ours). **It's acceptable as-is:** steady play is ~110 fps, room transitions carry a
screen-wipe that masks a single dropped frame, and transitions are infrequent in real play. Bytecode
precompile would *not* help (it speeds cart parse, not `load_room`).

**So the perf roadmap simplifies:** don't chase GC or a bespoke transition fix. The only lever is general
z8lua speed, and it's only worth it if a hitch-free 60 fps becomes a hard requirement — profile first.

## Landed / reverted

- **Kept:** the `MEASURE_FPS` heap + spike-isolation instrumentation (a better perf tool).
- **Reverted:** the z8lua GC tuning (didn't fix the real spike; not worth a non-standard GC config for a
  situational gain) and the temp `scanInput` auto-drive.
