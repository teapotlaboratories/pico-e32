#!/usr/bin/env bash
# Grab a still frame from the bench camera (ESP-EYE) aimed at the panel under test.
# This is the hardware-in-the-loop verification step for display changes — see
# docs/hardware/pico-e32-bench-camera.md and .ai/AGENTS.md -> Verifying changes.
#
# Usage:
#   tools/capture_frame.sh [label]        # -> captures/<timestamp>[-label].jpg
#
# The camera host (ESP-EYE IP) comes from BENCH_CAM_HOST, or tools/bench_cam.env:
#   BENCH_CAM_HOST=192.168.1.42
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/tools/bench_cam.env" ] && . "$ROOT/tools/bench_cam.env"

HOST="${BENCH_CAM_HOST:-}"
if [ -z "$HOST" ]; then
  echo "error: BENCH_CAM_HOST not set (the ESP-EYE's IP)." >&2
  echo "  export BENCH_CAM_HOST=<ip>   or   cp tools/bench_cam.env.example tools/bench_cam.env" >&2
  echo "  The IP is printed over UART on boot: 'bench-cam ready: http://<ip>/capture'" >&2
  echo "  See docs/hardware/pico-e32-bench-camera.md" >&2
  exit 2
fi

label="${1:-}"
ts="$(date +%Y%m%d-%H%M%S)"
out="$ROOT/captures/${ts}${label:+-$label}.jpg"
mkdir -p "$ROOT/captures"

if ! curl -sf --max-time 15 "http://${HOST}/capture" -o "$out"; then
  echo "error: capture failed from http://${HOST}/capture" >&2
  echo "  is the ESP-EYE powered and on the network? try: curl -I http://${HOST}/capture" >&2
  rm -f "$out"
  exit 1
fi

# sanity: a JPEG starts with FFD8
if [ "$(head -c2 "$out" | xxd -p)" != "ffd8" ]; then
  echo "warning: $out does not look like a JPEG" >&2
fi
printf '%s\n' "$out"
