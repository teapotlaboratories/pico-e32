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
import serial, time


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


def run(port, parse_line, segments, *, settle=8, lead=2, steps_per_frame=2,
        start_sends=(), timeout=42.0, done_grace=2.0, verbose=True):
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
    # reset into a normal boot: RTS=EN pulse low->high, DTR=GPIO0 held high
    p.dtr = False; p.rts = True
    time.sleep(0.15); p.reset_input_buffer(); p.rts = False

    step = steps_per_frame
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
    p.close()
    return cleared
