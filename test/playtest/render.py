#!/usr/bin/env python3
"""Render a Trace playing on the sim to an mp4 — cart-agnostic (the sim half of the video pipeline).

Plays each segment (a cart-provided `reset`, then the segment's per-game-frame masks), captures every
game-frame, and encodes to mp4 at the game's logical rate. The device counterpart is
[`tools/record_video.sh`](../../tools/record_video.sh) (bench camera). Needs ffmpeg + ImageMagick.

    import render, fake08sim as VM
    render.render("assets/celeste.p8", trace,
                  reset=lambda seg: VM.spawn(int(seg.meta['rx']), int(seg.meta['ry'])),
                  out="sim.mp4",
                  stop_on_clear=lambda st, seg: (st['rx'], st['ry']) != (int(seg.meta['rx']), int(seg.meta['ry'])))
"""
import os, sys, subprocess, tempfile, shutil

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake08-sim"))
import fake08sim as VM


def render(cart_path, trace, reset, out, *, scale=4, fps=None, hold=(15, 10), stop_on_clear=None):
    """Init `cart_path`, play `trace` on the sim, write `out` (mp4). Returns (out, frame_count). Callables
    are cart-supplied:
      reset(segment)             put the VM at the segment's start (e.g. VM.spawn(rx, ry)).
      stop_on_clear(state, seg)  optional -> bool: end the segment early (e.g. the room advanced), so the
                                 video doesn't run past the clear into the next room's frames.
    scale = nearest-neighbour pixel upscale; fps defaults to 60/steps_per_frame (30 for a 30 fps cart);
    hold = (frames before, frames after) each segment, for legibility."""
    fps = fps or max(1, round(60 / trace.steps_per_frame))
    VM.init(cart_path)
    tmp = tempfile.mkdtemp(prefix="render_"); fi = 0

    def dump():
        nonlocal fi
        VM.draw()
        ppm = b"P6\n128 128\n255\n" + bytes(VM.frame_rgb())
        subprocess.run(["convert", "ppm:-", "-scale", f"{128 * scale}x{128 * scale}",
                        os.path.join(tmp, f"{fi:06d}.png")], input=ppm, check=True)
        fi += 1

    try:
        for seg in trace.segments:
            reset(seg)
            for _ in range(hold[0]): dump()
            for m in seg.frames:
                VM.step_mask(m); dump()
                if stop_on_clear and stop_on_clear(VM.read(), seg):
                    break
            for _ in range(hold[1]): dump()
        subprocess.run(["ffmpeg", "-y", "-framerate", str(fps), "-i", os.path.join(tmp, "%06d.png"),
                        "-r", str(fps), "-pix_fmt", "yuv420p", "-movflags", "+faststart", out],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return out, fi
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
