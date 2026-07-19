#!/usr/bin/env python3
"""Host-side smoke test for the orchestrator (M8): the shared `orchestrate()` runs the solved Celeste trace
through the sim (device hooks omitted), clears both rooms, writes a well-formed report.json, and produces a
sim video. Proves the one-call report path without a board or a camera.

Needs `make -C test/playtest/fake08-sim` (libfake08sim.so) + ffmpeg/ImageMagick. SKIPs if the .so isn't
built (standalone exits 0 with a SKIP note; under pytest calls pytest.skip).

Run: `python3 test/playtest/test_orchestrate.py`  (exit 0 = pass/skip, 1 = fail), or via pytest."""
import sys, os, json, tempfile, shutil

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


def test_orchestrate_sim_report():
    VM = _load_vm()
    import orchestrate as O
    from trace import Trace
    if not shutil.which("ffmpeg") or not shutil.which("convert"):
        try:
            import pytest; pytest.skip("ffmpeg/ImageMagick not on PATH")
        except ImportError:
            print("SKIP: ffmpeg/ImageMagick not on PATH"); return
    tr = Trace.load(os.path.join(HERE, "celeste", "solution.trace.json"))
    reset = lambda seg: VM.spawn(int(seg.meta["rx"]), int(seg.meta["ry"]))
    stop = lambda st, seg: (st["rx"], st["ry"]) != (int(seg.meta["rx"]), int(seg.meta["ry"]))
    out = tempfile.mkdtemp(prefix="orch_test_")
    try:
        rep = O.orchestrate(os.path.join(REPO, "assets", "celeste.p8"), tr, out,
                            sim_reset=reset, sim_stop=stop, device_replay=None, verbose=False)
        assert rep["pass"] is True, "sim orchestration should pass"
        assert rep["sim"]["cleared"] == rep["sim"]["total"] == len(tr.segments), "every room should clear on the sim"
        assert "device" not in rep, "device section must be absent when no device hooks are given"
        # report.json is written and matches the returned dict
        on_disk = json.load(open(os.path.join(out, "report.json")))
        assert on_disk == rep, "report.json should mirror the returned report"
        assert os.path.getsize(rep["sim"]["video"]) > 0, "sim video should be non-empty"
    finally:
        shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    try:
        import fake08sim  # noqa: F401 — loads libfake08sim.so; OSError if not built
    except OSError as e:
        print(f"SKIP: sim not built (run `make -C test/playtest/fake08-sim`): {e}"); sys.exit(0)
    try:
        test_orchestrate_sim_report()
        print("  ok   test_orchestrate_sim_report\n1/1 passed"); sys.exit(0)
    except AssertionError as e:
        print(f"  FAIL: {e}"); sys.exit(1)
