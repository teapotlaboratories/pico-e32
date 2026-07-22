#!/usr/bin/env python3
"""Unit tests for the fc-scheduled command protocol + device scheduler (fc_sched.py). Pure Python, no VM —
runs standalone (exit 0/1) or under pytest. Mirrors what the firmware backend input_scheduled.c must do."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fc_sched import encode_cmd, parse_stream, DeviceScheduler, SYNC

L, R, U, D, O, X = 1, 2, 4, 8, 16, 32


def test_protocol_roundtrip():
    b = encode_cmd(1234, R | U | X, 1)
    assert len(b) == 8 and b[0] == SYNC
    cmds, rest = parse_stream(b)
    assert rest == b"" and cmds == [dict(fc=1234, mask=R | U | X, hold=1)]


def test_resync_past_junk_and_bad_frame():
    stream = b"log line\n" + encode_cmd(10, R, 1) + b"\xA6\x00bad" + encode_cmd(20, X, 2)
    cmds, _ = parse_stream(stream)
    assert [c["fc"] for c in cmds] == [10, 20]


def test_packet_split_across_reads():
    pk = encode_cmd(99, O, 1)
    c1, r1 = parse_stream(pk[:5])
    c2, r2 = parse_stream(r1 + pk[5:])
    assert c1 == [] and [c["fc"] for c in c2] == [99] and r2 == b""


def test_scheduler_apply_expire_miss():
    s = DeviceScheduler()
    s.feed(encode_cmd(100, R, 1), cur_fc=96)          # arrives in time (target 100 > now 96)
    assert s.poll(98) == 0                            # not yet
    assert s.poll(100) == R                           # applies at the target fc
    assert s.poll(102) == 0                           # expired after 2*hold
    s.feed(encode_cmd(200, X, 2), cur_fc=204)         # arrives after its frame -> miss
    assert s.miss == 1
    assert s.poll(200) == 0                           # a missed command never applies


def test_scheduler_arrival_on_target_frame_applies():
    s = DeviceScheduler()
    s.feed(encode_cmd(100, R, 1), cur_fc=100)         # drained exactly on its target frame -> on time
    assert s.miss == 0
    assert s.poll(100) == R


def test_scheduler_multiframe_hold():
    s = DeviceScheduler()
    s.feed(encode_cmd(300, R, 3), cur_fc=299)
    assert [s.poll(fc) for fc in (300, 302, 304, 306)] == [R, R, R, 0]   # active [300, 306)


def test_scheduler_or_of_concurrent_commands():
    s = DeviceScheduler()
    s.feed(encode_cmd(400, R, 2), cur_fc=399)
    s.feed(encode_cmd(400, U, 1), cur_fc=399)
    assert s.poll(400) == (R | U)                     # both active this frame
    assert s.poll(402) == R                           # U expired, R still held


def _main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t(); print(f"  ok  {t.__name__}")
    print(f"\n{len(tests)} fc-scheduled protocol/scheduler tests PASS")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
