#!/usr/bin/env python3
"""Render the sim playing the two solved rooms (100 M -> 200 M -> 300 M) to an mp4 — the pixel-perfect
device VM, for side-by-side comparison with the bench-camera video. Usage: python3 render_run.py <out.mp4>"""
import sys, os, re, ast, ctypes, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake08sim as S
S._lib.sim_draw.argtypes = []
REPO = "/home/argonite/Developments/pico-e32"

src = open(os.path.join(REPO, "tools", "celeste_playtest.py")).read()
def emb(n): return ast.literal_eval("[" + re.search(n + r" = \[(.*?)\n\]", src, re.S).group(1) + "]")
ROOMS = [((0, 0), emb("PLAN_100M")), ((1, 0), emb("PLAN_200M"))]

out = sys.argv[1] if len(sys.argv) > 1 else "sim_run.mp4"
frames_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vframes")
os.makedirs(frames_dir, exist_ok=True)
for f in os.listdir(frames_dir):
    os.remove(os.path.join(frames_dir, f))

S.init(os.path.join(REPO, "assets", "celeste.p8"))
fi = 0
def dump():
    global fi
    S._lib.sim_draw()
    open(os.path.join(frames_dir, "f.ppm"), "wb").write(b"P6\n128 128\n255\n" + S.frame_rgb())
    subprocess.run(["convert", os.path.join(frames_dir, "f.ppm"), "-scale", "512x512",
                    os.path.join(frames_dir, f"{fi:05d}.png")], check=True)
    fi += 1

for (rx, ry), plan in ROOMS:
    st = S.spawn(rx, ry)                     # wait for the player at this room's spawn
    for _ in range(8):                       # brief hold at spawn
        S.step(''); dump()
    for keys in plan:
        S.step(keys); dump()
        r = S.read()
        if (r['rx'], r['ry']) != (rx, ry):
            break
    for _ in range(6):                       # brief hold after clearing
        S.step(''); dump()

subprocess.run(["ffmpeg", "-y", "-framerate", "30", "-i", os.path.join(frames_dir, "%05d.png"),
                "-r", "30", "-pix_fmt", "yuv420p", "-movflags", "+faststart", out],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(f"wrote {out} ({fi} frames)")
