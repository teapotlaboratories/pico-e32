#!/usr/bin/env python3
"""Live CLOSED-LOOP control — the feedback counterpart to the open-loop Trace (trace.py + render.py/harness.py).

A solver agent's solution can take one of two shapes:

  * an **open-loop Trace** (per-game-frame button masks) — deterministic, best for SHORT / DISCRETE / self-
    checking tasks (Celeste rooms). Replays via render.py (sim) + harness.py (device, frame-synced).
  * a **live closed-loop POLICY** — `policy(state) -> mask`: read the game state each frame and react. Best
    for CONTINUOUS control (racing, balancing) where an open-loop replay would compound small errors on
    hardware (no feedback → drift). Runs via THIS module on BOTH targets: `drive_sim` reads state off the VM;
    `drive_device` reads the SAME state off serial telemetry and sends buttons live. Feedback self-corrects,
    so rnd/fps/timing drift is absorbed — the policy is byte-identical on both sides; only the state SOURCE
    differs.

The contract a cart supplies (all pure/cart-specific; the runners here are cart-agnostic):
  policy(state) -> int mask   uses ONLY fields the DEVICE telemetry streams (the device is the constraint).
  is_done(state) -> bool      the run is over (may be a stateful closure, e.g. "clock hit 0 after starting").
  read() -> state             SIM ONLY: the state dict via fake08sim sim_exec/sim_peek.
  parse_line(line) -> state   DEVICE ONLY: the SAME dict shape, parsed from a telemetry line (None to skip).

Because `drive_sim` can RECORD the masks the policy emits, running the policy on the sim is also how a cart
turns a closed-loop controller into an open-loop Trace (that's exactly what a solve.py does). Same policy,
three uses: solve+record (sim), verify/drive (sim), drive live (device)."""
import os, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake08-sim"))
import fake08sim as VM


def drive_sim(cart, policy, read, is_done, *, reset=None, max_frames=8000, record=False):
    """Run `policy` live on the sim: init `cart`, then each game-frame read the state, stop if `is_done`, else
    apply `policy(state)`. `reset(None)` runs once after init (e.g. srand to pin rnd). If `record`, also
    collect the per-frame mask list (a Trace segment's `frames`). Returns dict(final=state, frames=[…]|None,
    count=n)."""
    VM.init(cart)
    if reset:
        reset(None)
    frames = [] if record else None
    st = read()
    n = 0
    for _ in range(max_frames):
        if is_done(st):
            break
        m = policy(st)
        VM.step_mask(m); n += 1
        if record:
            frames.append(m)
        st = read()
    return dict(final=st, frames=frames, count=n)


def drive_device(port, policy, parse_line, is_done, *, timeout=180, telemetry_config=None,
                 fps_out=None, verbose=True, binary=None):
    """Run `policy` live on the board over serial: each telemetry frame, read the state, stop if `is_done`,
    else send `policy(state)`'s keys. Feedback makes it robust to rnd/fps/timing drift (unlike an open-loop
    replay). `telemetry_config` (a Lua tail expression) is sent to a cart-agnostic firmware at startup — what
    to stream — so the board needs no per-cart build.

    `binary=(sync, packet_len, unpack)` switches the reader from ASCII lines to FIXED-SIZE sync-framed binary
    packets — the board ships raw int16s (fewer bytes, no fix32->string/GC per frame) prefixed by `sync`.
    `unpack(pkt) -> state` stays cart-specific (the exact role `parse_line` plays for text), so this runner
    never learns the cart's struct; interleaved ASCII log lines are skipped for free (they hold no `sync`).
    When set, `parse_line` is unused. Returns dict(final=state, count=n)."""
    import serial
    from harness import FpsMeter, send_telemetry_config
    from trace import mask_to_keys
    p = serial.Serial(port, 115200, timeout=0.005)
    # reset into a normal boot (RTS=EN pulse, DTR=GPIO0 high) so the run starts from the title, deterministically
    p.dtr = False; p.rts = True
    time.sleep(0.15); p.reset_input_buffer(); p.rts = False
    send_telemetry_config(p, telemetry_config)   # if the firmware asks (CFG?), tell it what to stream
    meter = FpsMeter(2, 30)

    if binary is None:
        def extract(buf):                        # ASCII: one state per newline-terminated telemetry line
            states = []
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                s = parse_line(line)
                if s is not None:
                    states.append(s)
            return states, buf
    else:
        sync, plen, unpack = binary
        def extract(buf):                        # binary: one state per sync-framed fixed-size packet
            states = []
            while True:
                i = buf.find(sync)
                if i < 0:                        # no sync: drop all but a trailing byte (sync may straddle reads)
                    return states, buf[-1:]
                if len(buf) - i < plen:          # sync found but packet incomplete: keep from the sync on
                    return states, buf[i:]
                pkt = buf[i:i + plen]; buf = buf[i + plen:]
                s = unpack(pkt)
                if s is not None:
                    states.append(s)

    buf = b""; t0 = time.monotonic(); last_fc = -1; st = None; n = 0
    try:
        while time.monotonic() - t0 < timeout:
            chunk = p.read(p.in_waiting or 1)
            if chunk:
                buf += chunk
            states, buf = extract(buf)
            for s in states:
                meter.add(s.get("fc"), s.get("step"), s.get("draw"))
                if s.get("fc") == last_fc:        # one telemetry frame per Step; act once per Step
                    continue
                last_fc = s.get("fc"); st = s
                if is_done(s):
                    raise StopIteration
                keys = mask_to_keys(policy(s)); n += 1
                if keys:
                    p.write(keys.encode()); p.flush()
    except StopIteration:
        pass
    finally:
        p.close()
    if fps_out is not None:
        stats = meter.stats()
        if stats:
            fps_out.update(stats)
    if verbose:
        print(meter.summary())
    return dict(final=st, count=n)
