#!/usr/bin/env python3
"""fc-scheduled input — the frame-tagged command protocol + the device-side scheduler (host + sim twin).

This is the CART-AGNOSTIC infrastructure for closing the control loop over telemetry on frame-precise carts.
The current live path (`live.drive_device`) sends a button and the board applies it on whichever input-poll
catches it — a ~1-frame JITTERY latency that is fatal to Celeste's pixel-exact dashes (a dash one frame late
launches from the wrong pixel and dies). The fix is to stop letting transport luck pick the apply instant:

  * the host tags each command with a TARGET FRAME (the telemetry `fc` it already reads, + a lead k), and
  * the device applies it when its frame clock REACHES that fc — not when the byte happens to arrive.

Host-side jitter then only has to beat a deadline (arrive before the target frame); the apply is deterministic.
See the design in docs/runtime/pico-e32-fake08-input.md and the sim validation in
test/playtest/celeste/fc_latency.py.

This module is the single source of the wire format, used by BOTH ends:
  * `encode_cmd`  — the host encoder (and the firmware's expected layout).
  * `parse_stream` / `DeviceScheduler` — the device logic, mirrored here in Python so it can be developed and
    proven on the sim before it is ported to the firmware backend `components/input/input_scheduled.c`.

Wire format (host -> device), 8 bytes, little-endian, self-delimiting + checksummed:
    0xA6 | target_fc:u32 | mask:u8 | hold:u8 | csum:u8      (csum = XOR of bytes 0..6)
`target_fc` is in telemetry fc units (the board streams `fc`, which advances +2 per 30 fps game-frame); a
command holds `mask` for game-frames [target_fc, target_fc + 2*hold). A one-frame dash is hold=1."""
import struct

SYNC = 0xA6
PKT_LEN = 8


def encode_cmd(target_fc, mask, hold):
    """Host -> device: one 8-byte framed command (the exact bytes the firmware parses)."""
    body = struct.pack("<BIBB", SYNC, target_fc & 0xFFFFFFFF, mask & 0xFF, hold & 0xFF)
    csum = 0
    for b in body:
        csum ^= b
    return body + bytes([csum])


def parse_stream(buf):
    """Device: pull whole valid commands out of a byte stream (interleaved log/junk is skipped for free).
    Scans for SYNC, needs 8 bytes, verifies the XOR csum; a bad csum resyncs from the next byte. Returns
    (list-of-cmd-dicts, remaining_buf) — keep `remaining_buf` for the next read (a packet may straddle reads)."""
    cmds = []
    i, n = 0, len(buf)
    while True:
        while i < n and buf[i] != SYNC:
            i += 1
        if i >= n:
            return cmds, b""
        if n - i < PKT_LEN:
            return cmds, bytes(buf[i:])
        pkt = buf[i:i + PKT_LEN]
        csum = 0
        for b in pkt[:7]:
            csum ^= b
        if csum == pkt[7]:
            _, fc, mask, hold = struct.unpack("<BIBB", pkt[:7])
            cmds.append(dict(fc=fc, mask=mask, hold=hold))
            i += PKT_LEN
        else:
            i += 1


class DeviceScheduler:
    """The Python twin of the firmware backend (`input_scheduled.c`): feed raw bytes as they 'arrive' at a
    given device frame, then `poll(fc)` each frame for the held button mask. A command whose target frame has
    already passed when it arrives is a MISS (dropped + counted) — that miss rate is the on-device number the
    design turns on. One writer (the wire) / one reader (poll), so on the firmware this is a lock-free SPSC
    latch fed by the core-1 UART task."""

    def __init__(self):
        self.buf = b""
        self.table = []          # pending {fc, mask, hold}
        self.miss = 0            # commands that arrived after their target frame
        self.applied = 0         # commands that reached their active window
        self.fed = 0             # total valid commands received

    def feed(self, data, cur_fc):
        """Bytes land at device frame `cur_fc`. A command targeting a frame <= now already missed its slot."""
        self.buf += data
        cmds, self.buf = parse_stream(self.buf)
        for c in cmds:
            self.fed += 1
            if c["fc"] < cur_fc:               # its target frame was already produced (cur_fc past it) -> miss
                self.miss += 1
            else:                              # c.fc == cur_fc is on time (applied this frame); > is future
                self.table.append(c)

    def poll(self, fc):
        """Held mask for frame `fc`: OR of every command whose [fc, fc+2*hold) window covers it; expire past."""
        held = 0
        keep = []
        for e in self.table:
            end = e["fc"] + 2 * e["hold"]
            if e["fc"] <= fc < end:
                held |= e["mask"]
                if e["fc"] == fc:
                    self.applied += 1
                keep.append(e)
            elif fc < e["fc"]:
                keep.append(e)      # still future
            # else: expired -> drop
        self.table = keep
        return held
