#!/usr/bin/env python3
"""De-distort + upright a bench-camera frame.

The bench camera (M5Stack Timer Camera F / OV3660) has a wide, cheap lens: straight panel edges bow
outward ("barrel"/fisheye look). It is also mounted 90° rotated (bench-rig: "LEFT of frame = TOP of
panel"). This removes the radial distortion (OpenCV `undistort`) and rotates the frame upright, so a
capture/record reads like a head-on screenshot instead of a curved, sideways photo.

The lens model is a single radial term k1 (k2 optional) about the image centre with focal length
fx=fy=`fscale`*width. Coefficients live in tools/bench_cam.env (BENCH_CAM_K1/K2/…) so the still and
video paths share ONE calibration; defaults below are the tuned values for this rig.

    tools/undistort.py IN.jpg OUT.jpg            # env/default coeffs
    tools/undistort.py --k1 -0.28 IN.jpg OUT.jpg # override for tuning
    cat IN.jpg | tools/undistort.py - -          # stdin -> stdout (what capture_frame.sh uses)
"""
import argparse, os, sys
import numpy as np
import cv2

# Tuned for the pico-e32 bench rig (straightens the LCD bezel + the game's HUD bar). Override via env.
DEF_K1 = float(os.environ.get("BENCH_CAM_K1", "-0.36"))
DEF_K2 = float(os.environ.get("BENCH_CAM_K2", "0.0"))
DEF_FS = float(os.environ.get("BENCH_CAM_FSCALE", "1.0"))     # focal length = fscale * width
DEF_ROT = os.environ.get("BENCH_CAM_ROTATE", "cw")            # cw | ccw | 180 | none (coarse 90° for the mount)
DEF_FINE = float(os.environ.get("BENCH_CAM_FINE_ROTATE", "4.0"))  # extra CW degrees to level the residual tilt
DEF_ALPHA = float(os.environ.get("BENCH_CAM_ALPHA", "0.0"))   # 0=crop to valid pixels, 1=keep all (black corners)

_ROT = {"cw": cv2.ROTATE_90_CLOCKWISE, "ccw": cv2.ROTATE_90_COUNTERCLOCKWISE, "180": cv2.ROTATE_180}


def undistort(img, k1=DEF_K1, k2=DEF_K2, fscale=DEF_FS, alpha=DEF_ALPHA, rotate=DEF_ROT, fine=DEF_FINE):
    h, w = img.shape[:2]
    fx = fy = fscale * w
    K = np.array([[fx, 0, w / 2.0], [0, fy, h / 2.0], [0, 0, 1.0]])
    D = np.array([k1, k2, 0.0, 0.0, 0.0])
    newK, _ = cv2.getOptimalNewCameraMatrix(K, D, (w, h), alpha, (w, h))
    out = cv2.undistort(img, K, D, None, newK)
    if rotate in _ROT:
        out = cv2.rotate(out, _ROT[rotate])       # coarse 90° for the camera mount
    if fine:
        h2, w2 = out.shape[:2]                     # fine level: cv2 positive angle is CCW, so negate for CW
        M = cv2.getRotationMatrix2D((w2 / 2.0, h2 / 2.0), -fine, 1.0)
        out = cv2.warpAffine(out, M, (w2, h2), flags=cv2.INTER_LINEAR,
                             borderMode=cv2.BORDER_CONSTANT, borderValue=(0, 0, 0))
    return out


def main():
    ap = argparse.ArgumentParser(description="De-distort + upright a bench-camera frame")
    ap.add_argument("inp", help="input image, or - for stdin (JPEG)")
    ap.add_argument("out", help="output image, or - for stdout (JPEG)")
    ap.add_argument("--k1", type=float, default=DEF_K1)
    ap.add_argument("--k2", type=float, default=DEF_K2)
    ap.add_argument("--fscale", type=float, default=DEF_FS)
    ap.add_argument("--alpha", type=float, default=DEF_ALPHA)
    ap.add_argument("--rotate", default=DEF_ROT, choices=["cw", "ccw", "180", "none"])
    ap.add_argument("--fine", type=float, default=DEF_FINE, help="extra fine rotation, degrees CW")
    a = ap.parse_args()

    if a.inp == "-":
        buf = np.frombuffer(sys.stdin.buffer.read(), np.uint8)
        img = cv2.imdecode(buf, cv2.IMREAD_COLOR)
    else:
        img = cv2.imread(a.inp)
    if img is None:
        print("undistort: could not read input", file=sys.stderr); return 1

    out = undistort(img, a.k1, a.k2, a.fscale, a.alpha, a.rotate, a.fine)

    if a.out == "-":
        ok, enc = cv2.imencode(".jpg", out, [cv2.IMWRITE_JPEG_QUALITY, 95])
        if not ok:
            print("undistort: encode failed", file=sys.stderr); return 1
        sys.stdout.buffer.write(enc.tobytes())
    else:
        if not cv2.imwrite(a.out, out):     # imwrite picks the codec from the extension — a bad one fails here
            print(f"undistort: write failed for {a.out!r} (unrecognized image extension?)", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
