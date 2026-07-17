# 2026-07-16 — HG-6: `pal`, `camera`, `clip` — the drawing surface is complete

**Goal:** the last of the graphics surface — `pal()` (colour remap, Celeste's flash effect), `camera()`
(offset, Celeste's screen shake), and `clip()` (bounded drawing). With these, `spr`+`map`+`print`+`camera`+`pal`
cover every draw call a real Celeste `_draw` makes.

**Status:** ✅ **done and verified.** Built on the [#6 branch](2026-07-16-esp-lcd-vs-lovyangfx.md); the
work-hours window is open now, so this can land with the rest.

---

## One chokepoint, three effects

`camera`, `clip`, and `pal` all apply at the **single pixel-write function** `px()` — so every op
(`spr`/`map`/`print`/shapes) honours them without touching each drawer:

```c
static inline void px(int x,int y,int c){
    x-=g_cam_x; y-=g_cam_y;                                          // camera
    if(x<g_clip_x0||x>g_clip_x1||y<g_clip_y0||y>g_clip_y1) return;   // clip
    if((unsigned)x<128 && (unsigned)y<128) fb[y*128+x]=g_pal[c&15];  // pal remap
}
```

Defaults are the **identity** — zero camera, full-screen clip, identity palette — so nothing changes until
a cart sets them. That's why every earlier test stays byte-identical (below).

Semantics, matched to what Celeste actually uses (checked against `celeste.p8`):

- **`pal(c0,c1)`** — the **draw** palette: colour `c0` is drawn as `c1`. Celeste flashes with it (`pal(8,8)`,
  `pal(7,c)`, …). `pal()` resets the palette *and* transparency, as PICO-8 does. The `p=1` **screen**
  palette is *not* modelled — Celeste never uses it.
- **`camera([x],[y])`** — offsets all drawing by `(-x,-y)`. Celeste uses `camera(-2+rnd(5),…)` for screen
  shake. `camera()` resets.
- **`clip([x,y,w,h])`** — bounds drawing to a rectangle (screen space, after camera). Celeste never uses it,
  but it's cheap and part of the surface.

(`palt` — transparency — was already done in HG-3.)

## Verification — numeric, then on a real room

**Numeric** (`P8_GFXTEST=1`, read from the dumped framebuffer) — all pass:

```
clip inside (30,30)=green      fb= 3   (full-screen fill, clipped to an 8..55 box)
clip outside (60,60)=bg        fb= 0
pal remap 8->12 (90,18)        fb=12   (rectfill colour 8, drawn under pal(8,12))
pal control 8 (90,44)          fb= 8   (same rectfill, pal reset)
camera marker screen(68,68)    fb=10   (drawn at world 100,100 under camera(36,36))
camera: world(100,100)         fb= 0   (nothing there — it moved to screen space)
```

**On a real Celeste room** (`P8_MAPTEST` extended with `P8_CAMX`/`P8_CAMY`/`P8_FLASH`):

- `P8_CAMX=8 P8_CAMY=6` — the room renders **shifted** up-left by (8,6): camera works on real content
  (framebuffer differs from the un-shifted render).
- `P8_FLASH=1` (`for i=0,15 do pal(i,7) end`) — the whole room renders **white-on-black**: every colour
  remapped to 7, geometry preserved. That's exactly PICO-8's death-flash. Confirmed the only non-background
  colour in the frame is 7.

## No regression

Every prior test is **byte-identical** to before HG-6 — the identity defaults change nothing:

| test | fb_hash |
|---|---|
| trivial cart | `5e851a75` |
| HG-3 sprtest | `5e8744dd` |
| HG-4 maptest room 1 | `df144d44` |
| HG-5 texttest | `1b77de61` |

## How to reproduce

```sh
cd firmware/pico-e32-host/host && make
P8_GFXTEST=1 P8_DUMP=frames ./host && ../../../tools/p8_png.py frames/frame_000.raw gfx.png
P8_MAPTEST=1 P8_ROOM=1 P8_CAMX=8 P8_CAMY=6 P8_DUMP=frames ./host   # camera shift
P8_MAPTEST=1 P8_ROOM=1 P8_FLASH=1        P8_DUMP=frames ./host   # death-flash
```

## The surface is complete — what's next

**HG-1 through HG-6 are done.** `spr`+`map`+`print`+`camera`+`pal` is every draw call a real Celeste
`_draw` makes. Only HG-7 (`sspr`, stretched blit — low priority, Celeste barely uses it) remains on the
draw side.

The next milestone is bigger than one call: **run the actual Celeste cart end-to-end** and render a real
gameplay frame. That needs the cart's `_update` logic wired — `btn`, `mget`, `rnd`, the object system —
much of which the [luabench harness](../worklog/2026-07-14-phase0-gate2-luabench.md) already implements
(it runs Celeste's logic headless). Merging that input/logic layer with this draw layer is the step from
"every call works in isolation" to "Celeste plays."

## Board / commit state

Host-only (no device flashing). The pending commit now covers HG-3 → HG-6 + the on-panel demo + the font
header split. The board is still on the on-panel Celeste demo.
