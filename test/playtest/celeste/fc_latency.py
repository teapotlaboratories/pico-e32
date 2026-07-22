#!/usr/bin/env python3
"""Celeste closed-loop under control latency — the measurements behind the fc-scheduled input design.

Runs the room (0,0) closed-loop policy (closed_loop.py) on the sim under injected control lag, and validates
the fc-scheduled fix (../fc_sched.py) end-to-end BEFORE any firmware. Four experiments:

  latency_wall()   inject a fixed end-to-end lag N (apply the mask N frames after the state it saw) and sweep
                   N. Result: 0 frames clears; 1 frame dies at the up-left dash; any jitter dies. Celeste's
                   air-dashes are pixel-precise and error-fatal — closed-loop-over-serial is not viable.
  fatality()       drop each critical (dash/jump) command in turn: 5 of 7 are fatal-on-miss (the 3 dashes +
                   the 2 setup jumps); the 2 top hops self-recover (phase 6 re-pulses jump).
  predictor()      the host's LOCKSTEP TWIN (same policy from spawn, fed the committed commands) reproduces
                   the device's state k frames ahead EXACTLY — so it can decide the action for the target
                   frame it schedules. (VM savestate is the parked eris path here, so the twin, not a
                   snapshot, is the predictor.)
  fc_sweep()       drive the real VM through encode_cmd -> DeviceScheduler -> apply under a host round-trip
                   jitter model; a command missing its k-frame deadline is dropped. k=2 + the racer's serial
                   tuning (binary + 921600) -> 100% clear across realistic jitter.

    python3 test/playtest/celeste/fc_latency.py           # run all four, print the summary
"""
import os, sys, random
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))                 # fc_sched, live
sys.path.insert(0, os.path.join(HERE, "..", "fake08-sim"))   # fake08sim
sys.path.insert(0, HERE)                                     # closed_loop
import fake08sim as VM
import closed_loop as CL
from fc_sched import encode_cmd, DeviceScheduler

P_MS = 1000.0 / 30.0                 # game-frame period ~33.33 ms
X, O = 32, 16
_inited = False


def _ensure():
    global _inited
    if not _inited:
        VM.init(CL.CART); _inited = True


def _spawn_reset(pol):
    pol.reset(); VM.spawn(*CL.ROOM)


# --- the latency wall: closed-loop under a fixed end-to-end lag N ------------------------------------------
def _run_lagged(N, drop_frames=None, params=None):
    """Apply each mask N frames after the state it was computed from (a fixed round-trip lag). `drop_frames`:
    a set of frames whose critical press is dropped (a deadline miss). Returns dict(cleared, frames, final)."""
    _ensure(); pol = CL.Climber(params); _spawn_reset(pol)
    q = deque([0] * N); st = CL.read()
    for i in range(240):
        if CL.is_done(st):
            break
        m = pol(st)
        if drop_frames and i in drop_frames:
            m &= ~(X | O)                       # a dropped dash/jump command
        q.append(m)
        VM.step_mask(q.popleft()); st = CL.read()
    return dict(cleared=(st['rx'], st['ry']) != CL.ROOM, frames=i, final=(st['x'], st['y']))


def latency_wall():
    print("[latency wall] room (0,0) closed-loop, fixed end-to-end lag:")
    for N in range(0, 4):
        r = _run_lagged(N)
        print(f"    lag={N}: {'CLEAR at f'+str(r['frames']) if r['cleared'] else 'FAIL (died at '+str(r['final'])+')'}")


def _criticals():
    """Frames that issue a dash (X-edge) or jump (O-edge) command, from a clean run."""
    _ensure(); pol = CL.Climber(); _spawn_reset(pol)
    st = CL.read(); crit = []
    for i in range(240):
        if CL.is_done(st):
            break
        m = pol(st)
        if m & X: crit.append((i, 'dash'))
        elif m & O: crit.append((i, 'jump'))
        VM.step_mask(m); st = CL.read()
    return crit


def fatality():
    crit = _criticals()
    fatal = surv = 0
    print(f"[fatality] {len(crit)} critical commands; drop each one, does the room still clear?")
    for fr, kind in crit:
        r = _run_lagged(0, drop_frames={fr})
        if r['cleared']: surv += 1
        else: fatal += 1
        print(f"    drop f{fr:>3} {kind:<4} -> {'CLEARS (recovered)' if r['cleared'] else 'DIES '+str(r['final'])}")
    print(f"  => {fatal} fatal, {surv} survivable")


# --- lockstep-twin predictor ------------------------------------------------------------------------------
def _device_trajectory():
    _ensure(); pol = CL.Climber(); _spawn_reset(pol)
    st = CL.read(); states = [st]; masks = []
    for i in range(240):
        if CL.is_done(st):
            break
        m = pol(st); masks.append(m); VM.step_mask(m); st = CL.read(); states.append(st)
    return states, masks, (st['rx'], st['ry']) != CL.ROOM


def predictor(k=2, checks=(8, 20, 33, 55, 60)):
    states, masks, cleared = _device_trajectory()
    print(f"[predictor] lockstep twin (replay committed commands from spawn), k={k} frames ahead "
          f"(device clears: {cleared}):")
    allok = True
    for i in checks:
        if i + k >= len(states):
            continue
        _ensure(); CL.Climber(); VM.spawn(*CL.ROOM)
        for j in range(i + k):
            VM.step_mask(masks[j])
        s = CL.read()
        pred = (s['x'], s['y'], s['dj'], s['grnd'])
        a = states[i + k]; actual = (a['x'], a['y'], a['dj'], a['grnd'])
        ok = pred == actual; allok &= ok
        print(f"    twin@f{i:>3} -> f{i+k:<3}  pred={pred}  device={actual}  {'MATCH' if ok else 'DIVERGE'}")
    print(f"  => lockstep twin {'EXACT' if allok else 'DIVERGED'}")
    return allok


# --- fc-scheduled pipeline through the real protocol, under host jitter ------------------------------------
CONFIGS = {
    'tight     (binary+921600+low_latency)': dict(base=(1, 5),  p_spike=0.005, spike=(10, 30)),
    'realistic (binary, default USB timer)': dict(base=(3, 18), p_spike=0.02,  spike=(20, 45)),
    'loose     (ASCII, 115200, untuned)   ': dict(base=(8, 28), p_spike=0.05,  spike=(30, 70)),
}


def _sample_R(cfg, rng):
    r = rng.uniform(*cfg['base'])
    if rng.random() < cfg['p_spike']:
        r += rng.uniform(*cfg['spike'])
    return r


def _run_pipeline(k, cfg=None, rng=None):
    """Every game-frame: content = policy(state) (zero-lag, justified by the exact lockstep twin), encoded as
    a command for that frame, delivered with round-trip R. Missed (R >= k frames) -> the frame gets no input.
    Held inputs are re-commanded each frame, so only the one-frame-precise presses are miss-fatal."""
    _ensure(); pol = CL.Climber(); _spawn_reset(pol)
    sched = DeviceScheduler(); st = CL.read()
    for i in range(240):
        if CL.is_done(st):
            break
        fc = 2 * i
        pkt = encode_cmd(fc, pol(st), 1)
        late = cfg is not None and (_sample_R(cfg, rng) >= k * P_MS)
        sched.feed(pkt, cur_fc=(fc + 2) if late else (fc - 2))   # late -> drained AFTER its target frame -> miss
        VM.step_mask(sched.poll(fc)); st = CL.read()
    return dict(cleared=(st['rx'], st['ry']) != CL.ROOM, frames=i, miss=sched.miss, fed=sched.fed)


def fc_sweep(n=150):
    clean = _run_pipeline(k=2)
    print(f"[fc pipeline · clean] through encode->scheduler->apply: "
          f"{'CLEAR at f'+str(clean['frames']) if clean['cleared'] else 'FAIL'}, "
          f"{clean['fed']} commands, {clean['miss']} missed")
    print(f"[fc pipeline · jitter] clear-rate over {n} trials/cell (real protocol path):")
    print(f"  {'':41}k=1 (33ms)  k=2 (67ms)  k=3 (100ms)")
    for name, cfg in CONFIGS.items():
        cells = []
        for k in (1, 2, 3):
            rng = random.Random(4242)
            cells.append(sum(_run_pipeline(k, cfg, rng)['cleared'] for _ in range(n)) / n)
        print(f"  {name}  " + "  ".join(f"{c*100:6.1f}%" for c in cells))


def main():
    latency_wall(); print()
    fatality(); print()
    predictor(); print()
    fc_sweep()
    return 0


if __name__ == "__main__":
    sys.exit(main())
