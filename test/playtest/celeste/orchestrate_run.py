#!/usr/bin/env python3
"""Orchestrate a full Celeste play-test in one call: replay the solved trace on the SIM (verify + render a
video) and on the DEVICE (verify + on-board fps + a bench-camera video), folded into one report.json.
Thin Celeste adapter over ../orchestrate.py — it supplies the sim reset/clear callables and the device
replay/record hooks; the orchestration is generic.

    # sim + device (board = the CP2104 port), the full report:
    python3 test/playtest/celeste/orchestrate_run.py /dev/ttyUSB0 [--trace=<file>] [--out=<dir>]
    # sim only (no board attached):
    python3 test/playtest/celeste/orchestrate_run.py --sim-only [--trace=<file>] [--out=<dir>]

The device needs the TELEMETRY build (see celeste_playtest.py header) and the bench camera reachable
(tools/bench_cam.env). Artifacts (sim.mp4, device.mp4, report.json) land in --out (default /tmp/...);
they are throwaway per .ai/AGENTS.md — the committed deliverable is this code + the trace."""
import sys, os, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))            # ../orchestrate.py, ../render.py, ../trace.py
sys.path.insert(0, os.path.join(HERE, "..", "fake08-sim"))
sys.path.insert(0, HERE)                                # celeste_playtest.py (device replay hook)
import fake08sim as VM
import orchestrate as O
from trace import Trace
import celeste_playtest as CP


def main():
    argv = sys.argv[1:]
    sim_only = "--sim-only" in argv
    board = next((a for a in argv if not a.startswith("--")), None)
    tracefile = next((a.split("=", 1)[1] for a in argv if a.startswith("--trace=")),
                     os.path.join(HERE, "solution.trace.json"))
    outdir = next((a.split("=", 1)[1] for a in argv if a.startswith("--out=")), "/tmp/celeste-playtest")
    cart = os.path.join(REPO, "assets", "celeste.p8")
    tr = Trace.load(tracefile)

    # Celeste sim callables: spawn straight into the segment's room; a room is cleared when the grid advances.
    reset = lambda seg: VM.spawn(int(seg.meta["rx"]), int(seg.meta["ry"]))
    stop = lambda st, seg: (st["rx"], st["ry"]) != (int(seg.meta["rx"]), int(seg.meta["ry"]))

    device_replay = device_record = None
    if board and not sim_only:
        device_replay = lambda: CP.replay_device(board, tracefile)

        def device_record(out):
            subprocess.run(["tools/record_video.sh", "-o", out, "--", "python3",
                            "test/playtest/celeste/celeste_playtest.py", board, f"--trace={tracefile}"],
                           check=True, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    rep = O.orchestrate(cart, tr, outdir, sim_reset=reset, sim_stop=stop,
                        device_replay=device_replay, device_record=device_record)
    print(f"\nreport -> {os.path.join(outdir, 'report.json')}   PASS={rep['pass']}")
    return 0 if rep["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
