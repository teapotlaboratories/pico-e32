#!/usr/bin/env python3
"""ota_server.py — bench OTA endpoint for Settings -> SYSTEM UPDATE (WC-4a).

Serves one or more built firmware images plus a manifest for each, and prints the
CONFIG_PICO_E32_OTA_MANIFEST_URL line to paste into the build.

The manifest fields are read OUT OF THE IMAGE rather than typed in:

  target   <- chip id in the image header      (esp_image_header_t, uint16 @ 0x0C)
  version  <- app descriptor                   (esp_app_desc_t.version, @ 0x30)
  sha256   <- hash of the file on disk
  size     <- length of the file on disk

That is deliberate. A manifest that disagrees with its image is the failure this whole
feature has to avoid: the device refuses a wrong `target` up front, and a wrong sha256
or size aborts the update after a pointless 1.6 MB transfer. Deriving all four from the
binary makes those disagreements impossible to author by hand.

Usage:
    tools/ota_server.py build/pico-e32-fake08/*/pico_e32_fake08.bin
    tools/ota_server.py --port 8099 --host 192.168.7.181 <bin> [<bin> ...]

Then set the URL it prints (menuconfig, or a line in sdkconfig.defaults) and rebuild.
Ctrl-C to stop.

NOT FOR PRODUCTION. Plain HTTP by default, no image signing, and it serves whatever
directory it is given. It exists so an update can be exercised on the bench without
publishing anything. See docs/firmware/pico-e32-wifi-networking.md (WC-4a).
"""
import argparse, functools, hashlib, http.server, json, os, socket, struct, sys, tempfile

# esp_chip_id_t — components/esp_bootloader_format/include/esp_bootloader_desc.h
CHIP_IDS = {
    0x0000: "esp32",   0x0002: "esp32s2", 0x0005: "esp32c3", 0x0009: "esp32s3",
    0x000C: "esp32c2", 0x000D: "esp32c6", 0x0010: "esp32h2", 0x0012: "esp32p4",
}
IMAGE_MAGIC   = 0xE9
APP_DESC_OFF  = 0x20
APP_DESC_MAGIC = 0xABCD5432


def read_image_facts(path):
    """(target, version, project, built) straight out of the .bin, or raise ValueError."""
    with open(path, "rb") as f:
        head = f.read(0x100)
    if len(head) < 0x100 or head[0] != IMAGE_MAGIC:
        raise ValueError(f"{path}: not an ESP application image (magic {head[:1].hex()})")

    (chip_id,) = struct.unpack_from("<H", head, 0x0C)
    target = CHIP_IDS.get(chip_id)
    if target is None:
        raise ValueError(f"{path}: unknown chip id 0x{chip_id:04x}")

    (desc_magic,) = struct.unpack_from("<I", head, APP_DESC_OFF)
    if desc_magic != APP_DESC_MAGIC:
        raise ValueError(f"{path}: no app descriptor (magic 0x{desc_magic:08x})")

    def s(off, n):
        return head[off:off + n].split(b"\0", 1)[0].decode("utf-8", "replace")

    version = s(APP_DESC_OFF + 16, 32)
    project = s(APP_DESC_OFF + 48, 32)
    time_   = s(APP_DESC_OFF + 80, 16)
    date    = s(APP_DESC_OFF + 96, 16)
    return target, version, project, f"{date} {time_}".strip()


def lan_ip():
    """The address a board on the same network can actually reach — not 127.0.0.1."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))       # TEST-NET-1: routed, never answered
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description="Bench OTA endpoint for pico-e32.")
    ap.add_argument("images", nargs="+", help="built .bin file(s) — one per board is fine")
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--host", default=None, help="address to advertise (default: this machine's LAN IP)")
    ap.add_argument("--dir", default=None, help="serve from this directory (default: a temp dir)")
    args = ap.parse_args()

    host = args.host or lan_ip()
    root = args.dir or tempfile.mkdtemp(prefix="pico-e32-ota-")
    os.makedirs(root, exist_ok=True)

    print(f"serving from {root}\n")
    urls = []
    for img in args.images:
        try:
            target, version, project, built = read_image_facts(img)
        except ValueError as e:
            print(f"  SKIP {e}", file=sys.stderr)
            continue

        blob = open(img, "rb").read()
        bin_name = f"{project or 'firmware'}-{target}.bin"
        with open(os.path.join(root, bin_name), "wb") as f:
            f.write(blob)

        manifest = {
            "target":  target,
            "version": version,
            "build":   built,
            "url":     f"http://{host}:{args.port}/{bin_name}",
            "sha256":  hashlib.sha256(blob).hexdigest(),
            "size":    len(blob),
        }
        man_name = f"pico-e32-{target}.json"
        with open(os.path.join(root, man_name), "w") as f:
            json.dump(manifest, f)

        url = f"http://{host}:{args.port}/{man_name}"
        urls.append((target, version, url, len(blob)))
        print(f"  {target:<8} {version:<24} {len(blob):>9,} B   {bin_name}")

    if not urls:
        print("no usable images — nothing to serve", file=sys.stderr)
        return 1

    print("\nSet this in the build (idf.py menuconfig -> pico-e32 -> OTA manifest URL,")
    print("or add the line to firmware/pico-e32-fake08/sdkconfig.defaults), then rebuild.")
    print("Remember to delete the generated build/<app>/<board>/sdkconfig — sdkconfig.defaults")
    print("is only applied when that file does not yet exist.\n")
    for target, _v, url, _n in urls:
        print(f'  # {target}\n  CONFIG_PICO_E32_OTA_MANIFEST_URL="{url}"')
    print()

    # A board only offers the update when the manifest version differs from what it runs.
    # Re-serving the same build is a no-op by design, which is easy to mistake for a broken server.
    print("Note: the device compares version strings for inequality. Serving the build that is")
    print("already running is correctly reported as ALREADY UP TO DATE — bump the version")
    print("(firmware/pico-e32-fake08/version.txt) and rebuild to get a real update to test.\n")

    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=root)
    with http.server.ThreadingHTTPServer(("0.0.0.0", args.port), handler) as httpd:
        print(f"listening on 0.0.0.0:{args.port} — Ctrl-C to stop")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
