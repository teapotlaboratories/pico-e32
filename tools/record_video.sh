#!/usr/bin/env bash
# Record a video of the panel under test from the bench camera's MJPEG stream. Companion to
# tools/capture_frame.sh — see docs/hardware/pico-e32-bench-camera.md and .ai/AGENTS.md -> Verifying.
#
# VIDEO uses SVGA (800x600), STILLS use UXGA (1600x1200). The little ESP32 cam only makes ~6 UXGA fps
# but ~22 SVGA fps (each UXGA JPEG is ~103 KB vs ~23 KB), and for *motion* frame rate beats resolution.
# So: capture_frame.sh = sharp evidence stills (UXGA); this = smooth watchable video (SVGA). The frame is
# rotated 90° CW for the camera's mount and brightened for the dark PICO-8 cart, like a judged capture.
#
# Usage:
#   tools/record_video.sh [-t SECONDS] [-o OUT.mp4] [label]      # record for SECONDS (default 20)
#   tools/record_video.sh [-o OUT.mp4] -- <command...>           # record until <command> exits
# e.g. film the hands-free Celeste play-test end to end:
#   tools/record_video.sh -o /tmp/celeste.mp4 -- python3 test/playtest/celeste/celeste_playtest.py /dev/ttyUSB0
#
# The frame is de-distorted (the wide lens's barrel/fisheye bow), rotated 90° upright for the mount, and
# fine-rotated a few degrees CW to level the residual tilt — so the video reads head-on like a screenshot,
# matching tools/capture_frame.sh (which uses tools/undistort.py/OpenCV; ffmpeg's lenscorrection here is
# tuned to the same look, k1≈-0.22 ~ OpenCV's k1=-0.36, different parameterizations).
#
# Host from BENCH_CAM_HOST or tools/bench_cam.env (BENCH_CAM_HOST=<ip>). Tunables (env; set empty to disable):
#   VIDEO_SIZE=svga  VIDEO_LENS=cx=0.5:cy=0.5:k1=-0.22:k2=0 (de-distort)  VIDEO_ROTATE=1 (90° transpose)
#   VIDEO_FINE_ROTATE=4.0 (extra CW degrees, level)  VIDEO_CROP=WxH+X+Y (default none — whole frame)
#   VIDEO_EQ=brightness=0.05:saturation=1.3:contrast=1.08  VIDEO_SCALE=480:-2  VIDEO_FPS=20
#   VIDEO_TUNE='awb=0&exp=1200&gain=0&sat=2' (meter the dark panel)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/tools/bench_cam.env" ] && . "$ROOT/tools/bench_cam.env"
command -v ffmpeg >/dev/null || { echo "error: ffmpeg not found (needed to record/encode)." >&2; exit 3; }

HOST="${BENCH_CAM_HOST:-}"
if [ -z "$HOST" ]; then
  echo "error: BENCH_CAM_HOST not set (the bench camera's IP)." >&2
  echo "  export BENCH_CAM_HOST=<ip>   or   cp tools/bench_cam.env.example tools/bench_cam.env" >&2
  exit 2
fi

VIDEO_SIZE="${VIDEO_SIZE:-svga}"
VIDEO_LENS="${VIDEO_LENS-cx=0.5:cy=0.5:k1=-0.22:k2=0}"            # de-distort barrel; VIDEO_LENS= disables
VIDEO_ROTATE="${VIDEO_ROTATE-1}"                                   # transpose=1 = 90° CW mount; VIDEO_ROTATE= disables
VIDEO_FINE_ROTATE="${VIDEO_FINE_ROTATE-4.0}"                       # extra CW degrees to level the tilt; = disables
VIDEO_CROP="${VIDEO_CROP-}"
VIDEO_EQ="${VIDEO_EQ-brightness=0.05:saturation=1.3:contrast=1.08}"
VIDEO_SCALE="${VIDEO_SCALE:-480:-2}"
VIDEO_FPS="${VIDEO_FPS:-20}"
VIDEO_TUNE="${VIDEO_TUNE-awb=0&exp=1200&gain=0&sat=2}"

label=""; duration=""; out=""; cmd=()
while [ $# -gt 0 ]; do
  case "$1" in
    -t) duration="${2:?}"; shift 2;;
    -o) out="${2:?}"; shift 2;;
    --) shift; cmd=("$@"); break;;
    -h|--help) sed -n '2,20p' "$0"; exit 0;;
    -*) echo "unknown flag: $1" >&2; exit 2;;
    *) label="$1"; shift;;
  esac
done

# Videos are throwaway diagnostics -> /tmp, never the repo (.ai/AGENTS.md). -o or CAPTURE_DIR to keep one.
CAPTURE_DIR="${CAPTURE_DIR:-${TMPDIR:-/tmp}/pico-e32-captures}"
ts="$(date +%Y%m%d-%H%M%S)"
[ -z "$out" ] && out="$CAPTURE_DIR/${ts}${label:+-$label}.mp4"
mkdir -p "$(dirname "$out")"
log="$(mktemp)"

# Build the filter chain (de-distort -> rotate -> crop -> brighten -> scale), skipping any the user cleared.
# lenscorrection comes first, in the camera's native orientation (radial about the frame centre), before the
# 90° transpose — same order tools/undistort.py uses for the stills.
filters=()
[ -n "$VIDEO_LENS" ]        && filters+=("lenscorrection=${VIDEO_LENS}")
[ -n "$VIDEO_ROTATE" ]      && filters+=("transpose=${VIDEO_ROTATE}")
[ -n "$VIDEO_FINE_ROTATE" ] && filters+=("rotate=${VIDEO_FINE_ROTATE}*PI/180:c=black")  # CW level (ffmpeg +=CW)
[ -n "$VIDEO_CROP" ]   && filters+=("crop=${VIDEO_CROP}")
[ -n "$VIDEO_EQ" ]     && filters+=("eq=${VIDEO_EQ}")
filters+=("scale=${VIDEO_SCALE}")
vf="$(IFS=,; echo "${filters[*]}")"

# Lock exposure/AWB (and the size) once; /stream inherits whatever the last /capture set.
[ -n "$VIDEO_TUNE" ] && curl -s --max-time 20 "http://${HOST}/capture?size=${VIDEO_SIZE}&${VIDEO_TUNE}" -o /dev/null || true

url="http://${HOST}/stream?size=${VIDEO_SIZE}"
# NOTE: -t is an OUTPUT option — it must come BEFORE "$out", or ffmpeg ignores it and records forever.
ff=(ffmpeg -nostdin -y -loglevel warning -use_wallclock_as_timestamps 1 -f mpjpeg -i "$url"
    -vf "$vf" -r "$VIDEO_FPS" -c:v libx264 -pix_fmt yuv420p -crf 21)

rc=0
if [ ${#cmd[@]} -gt 0 ]; then
  echo "recording $VIDEO_SIZE -> $out  (until: ${cmd[*]})" >&2
  "${ff[@]}" -t 120 "$out" >"$log" 2>&1 &     # 120s safety cap in case the command hangs
  ffpid=$!
  sleep 1.2                                   # let ffmpeg connect before the action starts
  "${cmd[@]}" >&2 || rc=$?                     # child's stdout -> stderr, so OUR stdout is only the path
  sleep 0.6
  kill -INT "$ffpid" 2>/dev/null || true      # SIGINT so ffmpeg finalizes the moov atom cleanly
  wait "$ffpid" 2>/dev/null || true
else
  echo "recording $VIDEO_SIZE for ${duration:-20}s -> $out" >&2
  "${ff[@]}" -t "${duration:-20}" "$out" >"$log" 2>&1 || rc=$?
fi

if [ ! -s "$out" ]; then
  echo "error: no video written. ffmpeg log:" >&2; cat "$log" >&2; rm -f "$log"; exit 1
fi
rm -f "$log"
printf '%s\n' "$out"
exit "$rc"
