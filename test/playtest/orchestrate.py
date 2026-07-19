#!/usr/bin/env python3
"""The one-call orchestrator (cart-agnostic core): run a solution `Trace` through the SIM and the DEVICE and
fold everything into one report — per-segment clears on both sides, the on-device fps (achieved + headroom),
and a sim + device video. Cart adapters supply the sim `reset`/`stop` callables and the device replay/record
hooks; Celeste's entry is `celeste/orchestrate.py`. The report is what an operator (or an agent) reads per
cart. See test/playtest/README.md."""
import os, sys, json

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render as R


def orchestrate(cart_path, trace, outdir, *, sim_reset, sim_stop=None,
                device_replay=None, device_record=None, verbose=True):
    """Run `trace` through the sim (+ optionally the device) and return a report dict; write artifacts +
    `report.json` under `outdir`. Callables (cart-supplied):
      sim_reset(seg) / sim_stop(state, seg)   as render.py — reach a segment's start / detect its clear.
      device_replay() -> (cleared, total, fps_stats)   or None to skip the device.
      device_record(out_path)                           or None to skip the device video.
    """
    os.makedirs(outdir, exist_ok=True)
    rep = {"cart": os.path.basename(cart_path), "segments": [s.name for s in trace.segments],
           "frames": trace.total_frames()}

    # --- SIM: replay (verify) + render (video) ---
    sim_res = R.replay(cart_path, trace, sim_reset, sim_stop)
    sim_cleared = sum(1 for _, c, _ in sim_res if c)
    sim_mp4 = os.path.join(outdir, "sim.mp4")
    R.render(cart_path, trace, sim_reset, sim_mp4, stop_on_clear=sim_stop)
    rep["sim"] = {"cleared": sim_cleared, "total": len(sim_res), "video": sim_mp4,
                  "segments": [{"name": n, "cleared": c} for n, c, _ in sim_res]}
    if verbose:
        print(f"[sim]    {sim_cleared}/{len(sim_res)} cleared -> {sim_mp4}", flush=True)

    # --- DEVICE: replay (verify + fps) + record (bench-camera video) ---
    if device_replay is not None:
        dc, dt, fps = device_replay()
        dev = {"cleared": dc, "total": dt, "fps": fps}
        if verbose:
            fline = ""
            if fps:
                a, h = fps["achieved"], fps["headroom"]
                fline = (f"  fps achieved {a['min']:.1f}/{a['avg']:.1f}/{a['max']:.1f}"
                         f"  headroom {h['min']:.1f}/{h['avg']:.1f}/{h['max']:.1f}")
            print(f"[device] {dc}/{dt} cleared{fline}", flush=True)
        if device_record is not None:
            dev_mp4 = os.path.join(outdir, "device.mp4")
            device_record(dev_mp4); dev["video"] = dev_mp4
            if verbose:
                print(f"[device] recorded -> {dev_mp4}", flush=True)
        rep["device"] = dev

    rep["pass"] = (rep["sim"]["cleared"] == rep["sim"]["total"] and
                   ("device" not in rep or rep["device"]["cleared"] == rep["device"]["total"]))
    with open(os.path.join(outdir, "report.json"), "w") as f:
        json.dump(rep, f, indent=2)
    return rep
