#!/usr/bin/env python3
"""Host-side smoke test for live.py (the closed-loop runner): the racer's `policy` driven by `live.drive_sim`
actually makes progress on the exact VM — proving the live closed-loop path works without a board.

Needs `make -C test/playtest/fake08-sim` (libfake08sim.so) AND the (third-party, gitignored) racer cart
`assets/Pico Racer.p8.png`. SKIPs cleanly if either is absent (so CI without them passes): standalone exits 0
with a SKIP note; under pytest calls pytest.skip.

Run: `python3 test/playtest/test_live.py`  (exit 0 = pass/skip, 1 = fail), or via pytest."""
import sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "fake08-sim"))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "pico_racer"))

CART = os.path.join(REPO, "assets", "Pico Racer.p8.png")


def _skip(msg):
    try:
        import pytest; pytest.skip(msg)
    except ImportError:
        print("SKIP:", msg); sys.exit(0)


def test_live_drive_sim_makes_progress():
    try:
        import fake08sim  # noqa: F401 — loads libfake08sim.so; OSError if not built
    except OSError as e:
        _skip(f"sim not built (make -C test/playtest/fake08-sim): {e}")
    if not os.path.exists(CART):
        _skip(f"racer cart not present (third-party): {CART}")
    import live
    import racer_playtest as RP
    # the SAME policy the device uses, run live on the sim; it should drive past the start (tpos advances).
    r = live.drive_sim(CART, RP.policy, RP.racer_read, RP.make_done(), reset=RP._srand(39))
    tpos = r["final"]["tpos"]
    assert tpos > 3, f"closed-loop policy made no real progress on the sim (tpos={tpos})"
    assert r["count"] > 100, f"the run ended far too early ({r['count']} frames)"


if __name__ == "__main__":
    try:
        import fake08sim  # noqa: F401
    except OSError as e:
        print(f"SKIP: sim not built (run `make -C test/playtest/fake08-sim`): {e}"); sys.exit(0)
    if not os.path.exists(CART):
        print(f"SKIP: racer cart not present (third-party): {CART}"); sys.exit(0)
    try:
        test_live_drive_sim_makes_progress()
        print("  ok   test_live_drive_sim_makes_progress\n1/1 passed"); sys.exit(0)
    except AssertionError as e:
        print(f"  FAIL: {e}"); sys.exit(1)
