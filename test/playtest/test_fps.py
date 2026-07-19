#!/usr/bin/env python3
"""Host-side unit test for harness.FpsMeter — pure aggregation (no VM / hardware / serial port).

Run standalone (`python3 test/playtest/test_fps.py`, exit 0/1) or via pytest."""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import FpsMeter


def _feed(m, per_gf_compute_us):
    """Feed game-frames of known compute: each game-frame = steps_per_frame Steps summing to `compute`."""
    fc = 0
    for comp in per_gf_compute_us:
        half = comp // m.spf
        for k in range(m.spf):
            m.add(fc, comp - half * (m.spf - 1) if k == m.spf - 1 else half, 0); fc += 1


def test_grouping_and_stats():
    m = FpsMeter(steps_per_frame=2, target_fps=30)
    # 6 game-frames; drop=3 keeps the last three: 20000, 20000, 100000 us of compute.
    _feed(m, [20000] * 5 + [100000])
    s = m.stats(drop=3)
    assert s['frames'] == 3, s
    # headroom = 1e6/compute -> 50, 50, 10 ; achieved = min(30, headroom) -> 30, 30, 10
    assert abs(s['headroom']['min'] - 10.0) < 0.1 and abs(s['headroom']['max'] - 50.0) < 0.1, s
    assert abs(s['achieved']['min'] - 10.0) < 0.1 and abs(s['achieved']['max'] - 30.0) < 0.1, s
    assert abs(s['achieved']['avg'] - (30 + 30 + 10) / 3) < 0.1, s
    assert s['target'] == 30


def test_ignores_incomplete_and_none():
    m = FpsMeter(2, 30)
    m.add(0, None, None)        # missing timing -> ignored
    m.add(10, 5000, 5000)       # only 1 of 2 Steps for its game-frame -> incomplete, excluded
    assert m.stats() is None    # no complete game-frames
    assert "no timing" in m.summary()


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    fails = 0
    for t in tests:
        try:
            t(); print(f"  ok   {t.__name__}")
        except AssertionError as e:
            fails += 1; print(f"  FAIL {t.__name__}: {e}")
    print(f"{len(tests) - fails}/{len(tests)} passed")
    sys.exit(1 if fails else 0)
