#!/usr/bin/env python3
"""Host-side smoke test for the native VM (the shared gym): it builds + boots Celeste, the embedded room-0
plan clears the room on the exact VM, and the frame counter advances 2 per game-frame (host↔device clock).

Needs `make -C test/playtest/fake08-sim` (libfake08sim.so). If the .so isn't built the test SKIPs (so CI
without the native build passes): standalone exits 0 with a SKIP note; under pytest it calls pytest.skip.

Run: `python3 test/playtest/test_sim_smoke.py`  (exit 0 = pass/skip, 1 = fail), or via pytest."""
import sys, os, re, ast

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, "fake08-sim"))
sys.path.insert(0, HERE)


def _load_vm():
    try:
        import fake08sim as VM
        return VM
    except OSError as e:                       # libfake08sim.so not built
        try:
            import pytest; pytest.skip(f"sim not built (make -C test/playtest/fake08-sim): {e}")
        except ImportError:
            raise


def _plan_100m():
    # Read the embedded room-0 plan without importing celeste_playtest (which pulls in pyserial via harness).
    src = open(os.path.join(HERE, "celeste", "celeste_playtest.py")).read()
    return ast.literal_eval("[" + re.search(r"PLAN_100M = \[(.*?)\n\]", src, re.S).group(1) + "]")


def test_sim_clears_room0_and_frame_count():
    VM = _load_vm()
    from trace import keys_to_mask
    VM.init(os.path.join(REPO, "assets", "celeste.p8"))
    VM.spawn(0, 0)
    # the VM frame counter advances exactly 2 per game-frame (30 fps cart = 2 VM Steps) — the clock the
    # device streams in telemetry, so a host-built solution lines up.
    a = VM.frame_count(); VM.step_mask(0)
    assert VM.frame_count() - a == 2, "frame count should advance 2 per game-frame"
    # the embedded room-0 plan clears the room on the exact VM (replay-from-root)
    VM.spawn(0, 0); cleared = False
    for keys in _plan_100m():
        VM.step_mask(keys_to_mask(keys)); st = VM.read()
        if (st['rx'], st['ry']) != (0, 0):
            cleared = True; break
    assert cleared, "embedded room-0 plan did not advance the room on the sim"


if __name__ == "__main__":
    try:
        import fake08sim  # noqa: F401 — loads libfake08sim.so; OSError if not built
    except OSError as e:
        print(f"SKIP: sim not built (run `make -C test/playtest/fake08-sim`): {e}"); sys.exit(0)
    try:
        test_sim_clears_room0_and_frame_count()
        print("  ok   test_sim_clears_room0_and_frame_count\n1/1 passed"); sys.exit(0)
    except AssertionError as e:
        print(f"  FAIL: {e}"); sys.exit(1)
