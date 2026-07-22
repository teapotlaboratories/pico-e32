#!/usr/bin/env python3
"""Per-room SIM | DEVICE comparison video for a Celeste closed-loop chain, saved in the workspace
(test/playtest/celeste/videos/<name>.mp4). LEFT = the pixel-perfect twin render; RIGHT = the recovered board over
the bench camera (tools/record_video.sh) via OPEN-LOOP fc-scheduled replay. Same masks on both.

The device is slower (title-warmup + per-frame compute dips), so a naive side-by-side desyncs. This tool records
the device FIRST, capturing the wall-clock time it reaches each room's spawn, then renders the sim with pauses so
BOTH enter every room at the same moment: the sim plays a room at native speed, then freezes on that room's last
frame until the device finishes it. Result: the two are aligned at every room boundary.

    python3 test/playtest/celeste/render_compare.py all            # regenerate every video into celeste/videos/
    python3 test/playtest/celeste/render_compare.py 300m           # just the 100 M -> 200 M -> 300 M chain
    python3 test/playtest/celeste/render_compare.py 100m           # just 100 M
    PORT=/dev/ttyUSB0 python3 test/playtest/celeste/render_compare.py 200m --sim-only   # skip the board
Videos land in test/playtest/celeste/videos/<name>.mp4 (100m.mp4, 200m.mp4, 300m.mp4).
"""
import sys, os, subprocess, tempfile, json, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
PT = os.path.join(HERE, "..")
sys.path.insert(0, PT); sys.path.insert(0, os.path.join(PT, "fake08-sim")); sys.path.insert(0, HERE)
REPO = os.path.abspath(os.path.join(PT, "..", ".."))
VIDEODIR = os.path.join(HERE, "videos")
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FPS = 30
TAIL = 0.7          # seconds the bench recording runs past the clear (record_video.sh's post-command sleep)

import fake08sim as VM
import closed_loop as CL
import render as R
import gym

# name -> the room chain it plays (each row reaches its final room by clearing the earlier ones)
CHAINS = {"100m": ((0, 0),), "200m": ((0, 0), (1, 0)), "300m": ((0, 0), (1, 0), (2, 0)),
          "400m": ((0, 0), (1, 0), (2, 0), (3, 0))}


def sim_video(rooms, out, scale=4):
    """Render the sim chain (rooms) to mp4 at native speed (used for --sim-only / as a fallback)."""
    masks = CL.drive_sim_chain(rooms, record=True, verbose=False)["masks"]
    after = (int(rooms[-1][0]) + 1, int(rooms[-1][1]))

    class _Seg:
        def __init__(s, f): s.frames = f; s.meta = {}

    class _Tr:
        def __init__(s, f): s.segments = [_Seg(f)]; s.steps_per_frame = 2

    R.render(CL.CART, _Tr(masks), reset=lambda seg: VM.spawn(*rooms[0]), out=out, scale=scale,
             stop_on_clear=lambda st, seg: (int(st["rx"]), int(st["ry"])) == after)
    return out


def _sim_frames(rooms, tmpdir, scale=4):
    """Render every game-frame of the sim chain to a PNG; return (frame_paths, {room: first_frame_index})."""
    masks = CL.drive_sim_chain(rooms, record=True, verbose=False)["masks"]
    after = (int(rooms[-1][0]) + 1, int(rooms[-1][1]))
    VM.init(CL.CART); VM.spawn(*rooms[0])
    frames = []; frame_room = []; st = CL.read()
    for i, m in enumerate(masks):
        VM.draw(); pth = os.path.join(tmpdir, f"s{i:05d}.png")
        gym._rgb_to_png(VM.frame_rgb(), pth, scale); frames.append(pth)
        frame_room.append((int(st["rx"]), int(st["ry"])))
        VM.step_mask(m); st = CL.read()
        if (int(st["rx"]), int(st["ry"])) == after:
            VM.draw(); pth = os.path.join(tmpdir, f"s{i+1:05d}.png")
            gym._rgb_to_png(VM.frame_rgb(), pth, scale); frames.append(pth); frame_room.append(after); break
    start = {}
    for i, rc in enumerate(frame_room):
        start.setdefault(rc, i)
    return frames, start


def sim_video_synced(rooms, spawn_t, dev_dur, out, scale=4):
    """Render the sim padded so each room's spawn lands at the DEVICE's spawn time for that room (spawn_t, keyed
    by str(room)). Falls back to native sim_video if any room's device time is missing."""
    after = (rooms[-1][0] + 1, rooms[-1][1])
    dt = [spawn_t.get(str(tuple(rc))) for rc in rooms] + [spawn_t.get("clear")]
    if any(v is None for v in dt):
        print("  (device timing incomplete -> unsynced sim)"); return sim_video(rooms, out, scale)
    T = [dt[k + 1] - dt[k] for k in range(len(rooms))]          # device per-room durations (spawn->next spawn)
    pre = max(0.0, dev_dur - sum(T) - TAIL)                     # sim start pad = device pre-100M video time
    tmp = tempfile.mkdtemp(prefix="simframes_")
    frames, start = _sim_frames(rooms, tmp, scale); n = len(frames)
    if any(rc not in start for rc in rooms):        # sim chain didn't enter every room -> can't sync; fall back
        shutil.rmtree(tmp, ignore_errors=True)
        print("  (sim chain incomplete -> unsynced sim)"); return sim_video(rooms, out, scale)
    keys = list(rooms) + [after]
    seq = [0] * round(pre * FPS)                               # freeze at spawn until the device's 100 M starts
    for k, rc in enumerate(rooms):
        a = start[rc]; b = start.get(keys[k + 1], n)
        seq += list(range(a, b))                                # play room k at native speed
        seq += [b - 1] * max(0, round((T[k] - (b - a) / FPS) * FPS))   # then freeze until the device finishes it
    seqdir = os.path.join(tmp, "seq"); os.makedirs(seqdir)
    for j, fi in enumerate(seq):
        os.symlink(frames[fi], os.path.join(seqdir, f"{j:06d}.png"))
    subprocess.run(["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-framerate", str(FPS),
                    "-i", os.path.join(seqdir, "%06d.png"), "-c:v", "libx264", "-pix_fmt", "yuv420p",
                    "-crf", "18", out], check=True)
    shutil.rmtree(tmp, ignore_errors=True)          # drop the PNG frame set + symlink tree
    return out


def device_video(rooms, out, port=None):
    """Record the device open-loop chain via record_video.sh; also capture per-room spawn times. Returns
    (video_path, {str(room): spawn_wall_clock})."""
    port = port or os.environ.get("PORT", "/dev/ttyUSB0")
    cmd = ["python3", os.path.join(HERE, "fc_device.py"), port, "--openloop"]
    if len(rooms) == 1:
        cmd.append("--room0")
    elif len(rooms) == 3:
        cmd.append("--to300")
    elif len(rooms) == 4:
        cmd.append("--to400")
    tf = out + ".times.json"
    env = dict(os.environ, OPENLOOP_TIMES_FILE=tf)
    subprocess.run([os.path.join(REPO, "tools", "record_video.sh"), "-o", out, "--", *cmd],
                   check=True, cwd=REPO, env=env)
    spawn_t = json.load(open(tf)) if os.path.exists(tf) else {}
    return out, spawn_t


def _dur(path):
    return float(subprocess.check_output(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path]).strip())


def combine(sim_mp4, dev_mp4, out, height=600):
    """Side-by-side SIM | DEVICE, scaled to the same height, labelled, each padded (clone last frame) to the
    longer duration so neither cuts off early."""
    ds, dd = _dur(sim_mp4), _dur(dev_mp4)
    md = max(ds, dd)

    def lab(t):
        return (f"drawtext=fontfile={FONT}:text='{t}':x=(w-tw)/2:y=12:fontsize=22:fontcolor=white:"
                "box=1:boxcolor=black@0.6:boxborderw=8")

    fc = (f"[0:v]scale=-2:{height},tpad=stop_mode=clone:stop_duration={md-ds:.3f},{lab('SIM — twin, pixel-perfect')}[a];"
          f"[1:v]scale=-2:{height},tpad=stop_mode=clone:stop_duration={md-dd:.3f},{lab('DEVICE — board, bench cam')}[b];"
          f"[a][b]hstack=inputs=2")
    subprocess.run(["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-i", sim_mp4, "-i", dev_mp4,
                    "-filter_complex", fc, "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "21",
                    "-t", f"{md:.3f}", out], check=True)
    return out


def room_compare(name, sim_only=False, port=None):
    """Produce celeste/videos/<name>.mp4 = SIM | DEVICE for the chain CHAINS[name], synced at room spawns."""
    rooms = CHAINS[name]
    os.makedirs(VIDEODIR, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="compare_")
    out = os.path.join(VIDEODIR, f"{name}.mp4")
    try:
        if sim_only:
            sim = sim_video(rooms, os.path.join(tmp, "sim.mp4"))
            subprocess.run(["cp", sim, out.replace(".mp4", "-sim.mp4")], check=True)
            print(f"wrote {out.replace('.mp4', '-sim.mp4')} (sim only)"); return
        print(f"[{name}] recording device {list(rooms)} (open-loop) ...", flush=True)
        dev, spawn_t = device_video(rooms, os.path.join(tmp, "dev.mp4"), port=port)
        print(f"[{name}] rendering sim, synced to device room spawns ...", flush=True)
        sim = sim_video_synced(rooms, spawn_t, _dur(dev), os.path.join(tmp, "sim.mp4"))
        combine(sim, dev, out)
        print(f"wrote {out}")
        return out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)          # clean the compare_ scratch dir (sim.mp4/dev.mp4)


def main():
    if "--help" in sys.argv or "-h" in sys.argv:
        print(__doc__); return 0
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    name = args[0] if args else "300m"
    sim_only = "--sim-only" in sys.argv
    names = list(CHAINS) if name == "all" else [name]
    if any(n not in CHAINS for n in names):
        print(f"unknown room '{name}'; choose from {list(CHAINS)} or 'all'"); return 2
    for n in names:
        room_compare(n, sim_only=sim_only)
    print(f"\ndone -> {VIDEODIR}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
