#!/usr/bin/env bash
# Grab a still frame from the bench camera (M5Stack Timer Camera F) aimed at the panel under test.
# This is the hardware-in-the-loop verification step for display changes — see
# docs/hardware/pico-e32-bench-camera.md and .ai/AGENTS.md -> Verifying changes.
#
# STILLS are shot at QXGA (2048x1536) — the OV3660's full 3 MP array, so the small panel resolves as
# sharply as the sensor allows. (Video is the opposite: tools/record_video.sh uses SVGA for frame rate,
# since fps is bandwidth-bound at ~6 Mbit/s — see the bench doc.) Frame rate is irrelevant for a still,
# so max resolution wins. Forcing size=qxga here also undoes an earlier /stream?size=svga (aiming) that
# would otherwise leave the sensor small and every still soft.
#
# Usage:
#   tools/capture_frame.sh [label]        # -> /tmp/pico-e32-captures/<ts>[-label].jpg
#
# The camera host (the camera's IP) comes from BENCH_CAM_HOST, or tools/bench_cam.env:
#   BENCH_CAM_HOST=192.168.1.42
# CAP_QUERY= appends extra sensor params for this shot, e.g. the dark-cart tuning:
#   CAP_QUERY='awb=0&exp=1200&gain=0&sat=2' tools/capture_frame.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/tools/bench_cam.env" ] && . "$ROOT/tools/bench_cam.env"

HOST="${BENCH_CAM_HOST:-}"
if [ -z "$HOST" ]; then
  echo "error: BENCH_CAM_HOST not set (the bench camera's IP)." >&2
  echo "  export BENCH_CAM_HOST=<ip>   or   cp tools/bench_cam.env.example tools/bench_cam.env" >&2
  echo "  The IP is printed over UART on boot: 'bench-cam ready: http://<ip>/capture'" >&2
  echo "  See docs/hardware/pico-e32-bench-camera.md" >&2
  exit 2
fi

# Frames are throwaway diagnostics -> /tmp, never the repo (.ai/AGENTS.md -> Hardware & flashing
# notes). Override with CAPTURE_DIR=... only when a frame is deliberately being kept as evidence.
CAPTURE_DIR="${CAPTURE_DIR:-${TMPDIR:-/tmp}/pico-e32-captures}"

label="${1:-}"
ts="$(date +%Y%m%d-%H%M%S)"
out="$CAPTURE_DIR/${ts}${label:+-$label}.jpg"
mkdir -p "$CAPTURE_DIR"

# size=qxga guarantees full resolution regardless of what the sensor was last left at.
query="size=qxga${CAP_QUERY:+&${CAP_QUERY}}"
if ! curl -sf --max-time 20 "http://${HOST}/capture?${query}" -o "$out"; then
  echo "error: capture failed from http://${HOST}/capture?${query}" >&2
  echo "  is the bench camera powered and on the network? try: curl -I http://${HOST}/capture" >&2
  rm -f "$out"
  exit 1
fi

# sanity: a JPEG starts with FFD8
if [ "$(head -c2 "$out" | xxd -p)" != "ffd8" ]; then
  echo "warning: $out does not look like a JPEG" >&2
fi

# De-distort the wide lens (barrel/fisheye) + rotate upright, so the still reads like a head-on screenshot
# instead of a curved, sideways photo (the camera is mounted 90° rotated). tools/undistort.py (OpenCV) owns
# the coefficients. Set BENCH_CAM_UNDISTORT=0 to keep the raw sensor frame; on any failure the raw is kept.
if [ "${BENCH_CAM_UNDISTORT:-1}" = "1" ] && command -v python3 >/dev/null 2>&1; then
  und="${out%.jpg}.undist.jpg"   # keep a .jpg extension — OpenCV imwrite picks the codec from it
  if python3 "$ROOT/tools/undistort.py" "$out" "$und" && [ -s "$und" ]; then
    mv "$und" "$out"
  else
    rm -f "$und"
    echo "warning: undistort skipped (needs python3 + opencv-python + numpy); kept the raw frame" >&2
  fi
fi
printf '%s\n' "$out"
