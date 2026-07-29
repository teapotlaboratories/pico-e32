#!/usr/bin/env bash
# Lock the bench USB camera's exposure + white balance for filming an emissive LCD panel. UVC controls reset
# to auto on every replug, so re-run this after reconnecting the camera. See docs/hardware/pico-e32-bench-camera.md.
#   tools/usb_cam_setup.sh                 # exposure 60 (6ms) — tuned for a close-up of a bright panel
#   CAM_EXPOSURE=120 tools/usb_cam_setup.sh   # brighter, e.g. for a wider / dimmer framing
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
V="$HERE/v4l2ctl.py"
EXP="${CAM_EXPOSURE:-60}"   # Exposure Time Absolute, in units of 100us. ~60 = 6ms suits a close bright panel.
python3 "$V" set 0x009a0903 0        # Exposure, Dynamic Framerate -> off   (keeps a stable frame rate)
python3 "$V" set 0x009a0901 1        # Auto Exposure               -> Manual
python3 "$V" set 0x009a0902 "$EXP"   # Exposure Time, Absolute     -> $EXP
python3 "$V" set 0x0098090c 0        # White Balance, Automatic    -> off   (locked colour)
echo "bench cam configured: manual exposure ${EXP} (x100us), white balance locked, dynamic-framerate off"
