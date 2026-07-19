#!/usr/bin/env python3
"""The gym's eyes — an agent-facing observation layer over the fake08-sim VM.

A solver agent solves by LOOKING at the screen (the universal, cart-agnostic observation) and iterating.
This renders the current frame, or a sampled filmstrip of an input run, to viewable PNGs the agent can Read
in a single look — so it can see motion/progress, not just one still. Cart-agnostic: uses only the VM's
step/draw/frame_rgb/read. Needs ImageMagick (`convert`, `montage`) — already used by the bench tooling.

    import gym
    gym.snapshot("frame.png")                             # the current frame -> a PNG
    gym.run_filmstrip(masks, "run.png", every=6,          # replay masks, sample frames -> one contact-sheet
                      reset=lambda: VM.spawn(0, 0))        #   PNG (labelled with the game-frame index)
"""
import os, sys, subprocess, tempfile, shutil

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake08-sim"))
import fake08sim as VM

W = H = 128   # PICO-8 framebuffer


def _rgb_to_png(rgb, path, scale):
    """A 128x128 RGB buffer -> a nearest-neighbour-scaled PNG."""
    ppm = b"P6\n%d %d\n255\n" % (W, H) + bytes(rgb)
    subprocess.run(["convert", "ppm:-", "-scale", f"{W*scale}x{H*scale}", path],
                   input=ppm, check=True)
    return path


def snapshot(path, scale=4):
    """Render the current VM frame to `path` (scaled, nearest-neighbour). Returns the path."""
    VM.draw()
    return _rgb_to_png(VM.frame_rgb(), path, scale)


def run_filmstrip(masks, path, *, every=6, cols=8, scale=2, reset=None, label=None):
    """Replay `masks` on the VM, capture every `every`-th game-frame, and montage them into one PNG the agent
    can view at a glance. `reset` (e.g. lambda: VM.spawn(0,0)) runs first. `label(state, frame_idx)->str`
    annotates each tile (default: the game-frame index). Returns (path, [captured states])."""
    if reset:
        reset()
    tmp = tempfile.mkdtemp(prefix="gym_filmstrip_")
    try:
        tiles = []; states = []
        for i, m in enumerate(masks):
            VM.step_mask(m)
            if i % every == 0:
                VM.draw()
                st = VM.read(); states.append(st)
                tile = os.path.join(tmp, f"{len(tiles):04d}.png")
                _rgb_to_png(VM.frame_rgb(), tile, scale)
                txt = label(st, i) if label else f"f{i}"
                subprocess.run(["convert", tile, "-background", "black", "-fill", "white", "-pointsize", "11",
                                "-gravity", "South", "-splice", "0x14", "-annotate", "+0+1", txt, tile],
                               check=True)
                tiles.append(tile)
        subprocess.run(["montage", *tiles, "-tile", f"{cols}x", "-geometry", "+2+2",
                        "-background", "gray20", path], check=True)
        return path, states
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
