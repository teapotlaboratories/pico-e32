#!/usr/bin/env python3
"""Generic hands-free play-test harness for the fake-08 port — cart-agnostic.

Drives a cart through an ordered list of **segments** over serial, delivering each segment's per-frame
input plan FRAME-SYNCED to the firmware's telemetry frame counter, and self-verifies every segment. All
that a game supplies is a telemetry-line parser + its segments (each: a plan, a "reached the start"
detector, and a "cleared" detector) — everything cart-specific stays in the game's own module under
`test/playtest/<game>/` (see `celeste/celeste_playtest.py` for the reference user).

Why frame-synced: a room/segment often needs frame-precise inputs an open-loop timeline can't land. The
input is solved offline, then delivered locked to the frame counter the firmware streams. Requires the
firmware built with `-D TELEMETRY=1` (per-frame `T <frame> ...` over UART) and `-D INPUT_HOLD_FRAMES=1`
(each serial byte held exactly one frame). See docs/runtime/pico-e32-fake08-input.md.
"""
import serial, time, re


class Segment:
    """One stretch the harness drives blind.

    plan       per-GAME-frame input: the keys held that frame ('ru' / 'z' / '' ...). Bake any freeze
               frames into the plan so index == game-frame.
    at_start   state -> bool: True once the player is at this segment's start point. Delivery of `plan`
               begins `settle` game-frames after this first fires (frame 0 is locked to that counter).
    cleared    state -> bool: True once this segment is done; the harness advances to the next segment.
    describe   state -> str: the label logged on clear (defaults to `name`).
    """
    def __init__(self, name, plan, at_start, cleared, describe=None):
        self.name = name
        self.plan = plan
        self.at_start = at_start
        self.cleared = cleared
        self.describe = describe or (lambda st: name)


class FpsMeter:
    """Aggregate the telemetry's per-Step `(step_us, draw_us)` timing into game-frame fps. A game-frame is
    `steps_per_frame` host Steps; its compute is the sum of step+draw over those Steps. Given the cart's target
    fps, report min/avg/max of ACHIEVED (`min(target, ceiling)` — does the device hold the target rate?) and
    HEADROOM (the uncapped compute ceiling `1e6/compute`). The firmware times only the cart's Step+draw (not
    the telemetry poke), so headroom is unperturbed and achieved is derived from compute vs the frame budget —
    no wall-clock, so per-frame telemetry overhead doesn't skew the numbers."""
    def __init__(self, steps_per_frame=2, target_fps=30):
        self.spf = steps_per_frame
        self.target = target_fps
        self.gf = {}                        # game-frame idx -> [compute_us_sum, step_count]

    def add(self, fc, step_us, draw_us):
        if step_us is None or draw_us is None:
            return
        g = fc // self.spf
        c = self.gf.get(g)
        if c is None:
            self.gf[g] = [step_us + draw_us, 1]
        else:
            c[0] += step_us + draw_us; c[1] += 1

    def stats(self, drop=3):
        # complete game-frames only (all spf Steps arrived), dropping the first `drop` (boot/spawn spikes)
        comps = [c for _, (c, n) in sorted(self.gf.items()) if n == self.spf and c > 0][drop:]
        if not comps:
            return None
        head = [1e6 / c for c in comps]                     # headroom fps (uncapped ceiling)
        ach = [min(self.target, h) for h in head]           # achieved (held at target when compute fits)
        avg = lambda xs: sum(xs) / len(xs)
        return dict(frames=len(comps), target=self.target,
                    achieved=dict(min=min(ach), max=max(ach), avg=avg(ach)),
                    headroom=dict(min=min(head), max=max(head), avg=avg(head)))

    def summary(self):
        s = self.stats()
        if not s:
            return "fps: (no timing telemetry)"
        a, h = s['achieved'], s['headroom']
        return (f"fps over {s['frames']} game-frames (target {s['target']}): "
                f"achieved min/avg/max = {a['min']:.1f}/{a['avg']:.1f}/{a['max']:.1f}  |  "
                f"headroom min/avg/max = {h['min']:.1f}/{h['avg']:.1f}/{h['max']:.1f}")


def send_telemetry_config(p, config, timeout=6.0):
    """Runtime telemetry-tail handshake (for firmware built `-D TELEMETRY_HOST_CFG=1`): wait for the board's
    `CFG?` prompt after boot, then send `TT<config>\\n` — `config` is the cart's telemetry-TAIL Lua expression
    (what to stream after the generic `T <fc> <step> <draw> ` prefix). No-op if `config` is falsy. This is how
    a cart tells a cart-AGNOSTIC firmware what to stream, so no per-cart firmware edit is needed. Returns True
    if the tail was sent (the board fell back to its default otherwise)."""
    if not config:
        return False
    t0 = time.monotonic(); buf = b""
    while time.monotonic() - t0 < timeout:
        chunk = p.read(p.in_waiting or 1)
        if chunk:
            buf += chunk
        if b"CFG?" in buf:
            p.write(b"TT" + config.encode() + b"\n"); p.flush()
            _maybe_switch_baud(p)          # firmware (TELEMETRY_BAUD) may announce a higher run baud
            return True
    return False


def _maybe_switch_baud(p, timeout=2.0):
    """If the board announces `BAUD <n>` after the tail (TELEMETRY_BAUD firmware), switch the host serial to
    match it for the run. No-op if no announce arrives. Returns the new baud or None."""
    t0 = time.monotonic(); buf = b""
    while time.monotonic() - t0 < timeout:
        chunk = p.read(p.in_waiting or 1)
        if chunk:
            buf += chunk
        m = re.search(rb"BAUD (\d+)\r?\n", buf)
        if m:
            time.sleep(0.06)               # let the board finish switching first (it waits 40 ms, then flips)
            p.baudrate = int(m.group(1)); p.reset_input_buffer()
            return int(m.group(1))
    return None


def run(port, parse_line, segments, *, settle=8, lead=2, steps_per_frame=2,
        start_sends=(), timeout=42.0, done_grace=2.0, target_fps=None, fps_out=None,
        telemetry_config=None, verbose=True):
    """Drive `segments` over the serial `port`; return the number cleared.

    parse_line(line: bytes) -> dict | None
        Cart-specific telemetry parser. Must return a state dict containing at least `fc` (int frame
        counter); segment callbacks read whatever else they need. None for non-telemetry lines.
    settle            game-frames to idle at a segment's start before its plan frame 0.
    lead              firmware frames to send ahead of the target update (serial + poll latency).
    steps_per_frame   firmware frames per game-frame (2 = a 30 fps cart on the 60 Hz-resumed host;
                      1 for an `_update60` cart).
    start_sends       [(t_seconds, bytes), ...] sent early to get past a title screen.
    timeout           overall wall-clock budget (s); done_grace = extra seconds after the last clear.
    """
    p = serial.Serial(port, 115200, timeout=0.02)
    step = steps_per_frame
    meter = FpsMeter(steps_per_frame, target_fps if target_fps else max(1, round(60 / steps_per_frame)))
    try:
        # reset into a normal boot: RTS=EN pulse low->high, DTR=GPIO0 held high
        p.dtr = False; p.rts = True
        time.sleep(0.15); p.reset_input_buffer(); p.rts = False

        send_telemetry_config(p, telemetry_config)   # if the firmware asks (CFG?), tell it what to stream

        start_sends = list(start_sends)
        t0 = time.monotonic()
        si = 0
        cur = 0             # index into segments of the one being cleared
        plan_fc0 = None     # frame counter mapped to the current segment's plan index 0
        sent = set()
        cleared = 0
        done_t = None
        buf = b""
        while True:
            now = time.monotonic() - t0
            if now > timeout or (done_t is not None and now > done_t + done_grace):
                break
            while si < len(start_sends) and start_sends[si][0] <= now:
                p.write(start_sends[si][1]); p.flush(); si += 1
            # Drain the telemetry each iteration so the frame counter stays real-time (else the 60 Hz stream
            # backs up in the OS buffer and delivery lags on a later segment).
            chunk = p.read(p.in_waiting or 1)
            if chunk:
                buf += chunk
            latest = None
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                st = parse_line(line)
                if st is None:
                    continue
                latest = st
                meter.add(st['fc'], st.get('step'), st.get('draw'))
                if cur >= len(segments):
                    continue
                seg = segments[cur]
                if seg.cleared(st):
                    cleared += 1
                    if verbose:
                        print(f"CLEARED {seg.describe(st)}  at t={now:.2f}s")
                    cur += 1; plan_fc0 = None; sent = set()
                    if cur >= len(segments):
                        done_t = now
                    continue
                if plan_fc0 is None and seg.at_start(st):
                    plan_fc0 = st['fc'] + step * settle
            # deliver the current segment's plan against the latest (real-time) frame counter
            if latest is not None and cur < len(segments) and plan_fc0 is not None:
                plan = segments[cur].plan; fc = latest['fc']
                for f in range(len(plan)):
                    if f in sent or not plan[f]:
                        continue
                    if fc >= plan_fc0 + step * f - lead:
                        for c in plan[f]:
                            p.write(c.encode())
                        sent.add(f)
                p.flush()
    finally:
        p.close()
    s = meter.stats()
    if fps_out is not None and s:
        fps_out.update(s)
    if verbose:
        print(meter.summary())
    return cleared
