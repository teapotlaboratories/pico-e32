#!/usr/bin/env python3
"""Host-side unit tests for the Trace contract (test/playtest/trace.py) — pure Python, no VM/hardware.

Run standalone (`python3 test/playtest/test_trace.py`, exit 0/1) or via pytest (test_* functions)."""
import sys, os, json, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace import Trace, Segment, keys_to_mask, mask_to_keys, keys_seq_to_masks


def test_mask_keys_roundtrip():
    # The mask is the canonical form: keys -> mask -> keys -> mask must be stable (key order/'o'-vs-'z'
    # normalise, but the mask does not).
    for keys in ['', 'r', 'ru', 'rux', 'z', 'rz', 'x', 'lu', 'd', 'ux', 'lru d']:
        m = keys_to_mask(keys)
        assert keys_to_mask(mask_to_keys(m)) == m, f"{keys!r} -> {m} -> {mask_to_keys(m)!r}"
    assert keys_to_mask('o') == keys_to_mask('z') == 16      # O is one button (bit 16), 'z'/'o' alias it
    assert keys_to_mask('zx') == 16 | 32                     # jump + dash the same frame
    assert keys_to_mask('') == 0 and mask_to_keys(0) == ''   # empty frame


def test_keys_seq_to_masks():
    assert keys_seq_to_masks(['r', 'ru', '', 'z']) == [2, 2 | 4, 0, 16]


def test_trace_save_load_roundtrip():
    segs = [Segment("A", keys_seq_to_masks(['r', 'ru', '', 'z']), {"rx": 0, "ry": 0, "spawn": [8.0, 96.0]}),
            Segment("B", [1, 2, 4, 8, 16, 32], {"rx": 1, "ry": 0})]
    tr = Trace("celeste.p8", segs, steps_per_frame=2, meta={"solver": "test"})
    fd, path = tempfile.mkstemp(suffix=".json"); os.close(fd)
    try:
        tr.save(path)
        t2 = Trace.load(path)
        assert t2.cart == "celeste.p8" and t2.steps_per_frame == 2 and t2.meta == {"solver": "test"}
        assert len(t2.segments) == 2
        assert t2.segments[0].frames == segs[0].frames and t2.segments[0].meta == segs[0].meta
        assert t2.segments[1].frames == [1, 2, 4, 8, 16, 32]
        assert t2.total_frames() == tr.total_frames() == 10
    finally:
        os.remove(path)


def test_load_rejects_bad_version():
    fd, path = tempfile.mkstemp(suffix=".json"); os.close(fd)
    try:
        with open(path, "w") as f:
            json.dump({"version": 999, "cart": "x", "segments": [], "steps_per_frame": 2}, f)
        try:
            Trace.load(path)
            assert False, "should have rejected version 999"
        except ValueError:
            pass
    finally:
        os.remove(path)


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
