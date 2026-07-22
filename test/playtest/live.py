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

# Cap the predictive rebase's replay-from-root (O(gf) VM steps run inline in the serial loop): beyond this the
# stall would risk missing fc windows. Short chains never reach it; ~1500 frames is well past any Celeste chain.
REBASE_MAX_GF = 1500


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


def drive_device_predictive(port, make_policy, twin, parse_line, at_start, is_done, diverged, *,
                            encode_cmd, twin_done=None, has_state=None, k=2, settle=8, step=2, timeout=45.0,
                            max_frames=400, telemetry_config=None, resync=True, drop_frames=(), fps_out=None,
                            warmup=None, verbose=True):
    """TWIN-IN-THE-LOOP predictive closed loop on the board — the general closed-loop-on-hardware runner.

    The device applies frame-precise input over the fc-scheduled backend (INPUT_BACKEND=scheduled), which the
    naive read-telemetry-react-live loop can't (serial jitter is fatal to a pixel-exact dash). So instead the
    host runs a deterministic TWIN of the cart, leads the board by `k` frames, and streams each frame's mask as
    an fc-tagged command (encode_cmd) targeting its exact frame. The loop is CLOSED on the board: every
    telemetry frame the driver compares the board's real state to the twin's prediction and, on DIVERGENCE
    (a missed command / rnd / fps drift), REBASES the twin to the board's actual state (replay-from-root +
    `twin['seed']`) and RE-PLANS forward. Cart-agnostic — a cart supplies:

      make_policy() -> policy(state)->mask       a FACTORY (a rebase rebuilds the policy to reconstruct a
                                                 stateful phase machine's hidden phase; stateless is fine too)
      twin = dict(reset, read, step, seed)       reset()->start; read()->state; step(mask); seed(board_state)
      parse_line(bytes) -> board_state|None      device telemetry parser (must carry 'fc')
      at_start(board_state) -> bool              the board reached the plan's frame-0 state
      is_done(state) -> bool                     run over (cleared / dead)
      diverged(board_state, twin_state) -> bool  the board drifted off the twin's prediction

    `warmup(board_state, send)` (optional) drives the board from its reset state to the plan's frame-0 state,
    e.g. Celeste's title-skip: pulse jump fc-commands (`send(target_fc, mask, hold)`) until the player spawns.
    The racer needs none — its twin models the countdown, so the policy's own early masks are the start input.

    `drop_frames` (test only) silently withholds those commands to inject a divergence, so the rebase/recovery
    can be exercised on a deterministic cart. Returns dict(cleared, clear_fc, delivered, divergences, resyncs).
    """
    import serial
    from harness import send_telemetry_config, FpsMeter
    if has_state is None:                  # "this telemetry frame carries a live entity" (cart-specific; the
        has_state = lambda bs: bs.get("fc") is not None   # default accepts any parsed frame — racer has no pre-spawn gap)
    meter = FpsMeter(step, max(1, round(60 / step)))   # the cart's real Step+draw compute -> fps (input-shape-independent)
    p = serial.Serial(port, 115200, timeout=0.02)
    try:                                    # close the port if setup (reset pulse / telemetry config) raises
        p.dtr = False; p.rts = True
        time.sleep(0.2); p.reset_input_buffer(); p.rts = False
        send_telemetry_config(p, telemetry_config)
    except BaseException:
        p.close(); raise

    twin['reset']()
    pol = make_policy()                     # the live policy; rebuilt on a rebase to reconstruct its phase
    tmask = []                              # tmask[g] = mask applied at twin frame g
    tstate = [twin['read']()]              # tstate[g] = twin state at frame g (tstate[len(tmask)] = current)

    def grow(upto):                        # run the policy on the twin out to frame `upto` (bounded: never
        upto = min(upto, max_frames)       # step the twin PAST its own done-state — the fake08 VM is not
        while len(tmask) <= upto:           # teardown-safe across a clear/death, so running on would crash it)
            if twin_done is not None and twin_done(tstate[len(tmask)]):
                break
            m = pol(tstate[len(tmask)])
            tmask.append(m); twin['step'](m); tstate.append(twin['read']())

    def rebase(board_state, gf):           # replay the twin to frame gf, seed it to the board, re-plan
        nonlocal pol
        twin['reset']()
        for g in range(gf):
            twin['step'](tmask[g])
        twin['seed'](board_state)
        del tmask[gf:]; del tstate[gf + 1:]
        tstate[gf] = twin['read']()        # the board's ACTUAL state at gf (seeded into the twin)
        pol = make_policy()                # reconstruct the policy's phase by replaying it over frames 0..gf-1
        for g in range(gf):                # (the plan the board followed up to the divergence)
            pol(tstate[g])
        for g in [x for x in sent if x > gf]:   # future lead-window frames were just re-planned -> drop them from
            sent.discard(g)                     # `sent` so the delivery loop re-sends the CORRECTED masks

    plan_fc0 = None; sent = set(); latest = None; buf = b""
    cleared = False; clear_fc = None; divergences = 0; resyncs = 0; final = None
    t0 = time.monotonic()
    try:
        while time.monotonic() - t0 < timeout:
            chunk = p.read(p.in_waiting or 1)
            if chunk:
                buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                bs = parse_line(line)
                if bs is None:
                    continue
                latest = bs
                meter.add(bs.get("fc"), bs.get("step"), bs.get("draw"))   # fps from the cart's real compute
                if has_state(bs):
                    final = bs
                if plan_fc0 is None and warmup is not None:   # drive the board from reset to the frame-0 state
                    warmup(bs, lambda t, m, h=1: (p.write(encode_cmd(t, m, h)), p.flush()))
                if plan_fc0 is None and at_start(bs):
                    plan_fc0 = bs["fc"] + step * settle
                if plan_fc0 is not None and is_done(bs):
                    cleared = True; clear_fc = bs["fc"]
            if cleared:
                break
            if latest is None or plan_fc0 is None or not has_state(latest):
                continue
            F = latest["fc"]; gf = (F - plan_fc0) // step             # board game-frame (may be < 0 pre-start)
            if 0 <= gf < len(tstate) and diverged(latest, tstate[gf]):   # board drifted off the prediction
                divergences += 1
                # rebase replays the twin from root (O(gf); no VM savestate here), running inline in this read
                # loop -> a very late divergence would stall telemetry reads long enough to miss the next fc
                # windows and cascade. Cap it: short chains (Celeste) never hit it; the racer's lighter path is
                # live-reactive drive_device, not this predictive one.
                if resync and gf <= REBASE_MAX_GF:
                    rebase(latest, gf); resyncs += 1
            grow(max(gf, 0) + k)                                      # keep the plan k frames ahead of the board
            for g in range(len(tmask)):                              # deliver each command its k-frame lead early
                if g in sent:
                    continue
                target = plan_fc0 + step * g
                if F >= target - step * k:                          # (fires even before gf>=0, so the first
                    if g not in drop_frames:                        # drop_frames: withhold -> inject a miss
                        p.write(encode_cmd(target, tmask[g], 1))
                    sent.add(g)
            p.flush()
    finally:
        p.close()
    stats = meter.stats()
    if fps_out is not None and stats:
        fps_out.update(stats)
    if verbose:
        print(f"predictive: delivered {len(sent)} fc-commands, {divergences} divergence(s), {resyncs} rebase(s)")
        print(f"-> {'CLEAR at fc=' + str(clear_fc) if cleared else 'NO CLEAR'}")
        print(meter.summary())
    return dict(cleared=cleared, clear_fc=clear_fc, delivered=len(sent),
                divergences=divergences, resyncs=resyncs, final=final, fps=stats)


def _has_state(bs):
    """True if a telemetry frame carries a live entity (not the pre-spawn 'x' placeholders)."""
    return bs.get("x") is not None
