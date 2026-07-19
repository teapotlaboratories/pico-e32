"""A portable, replay-able solution 'trace' — the canonical output of a solve.

A solution is NOT a savestate or a solver-internal object: it is a plain per-game-frame sequence of held
buttons, grouped into the segments the run clears in order. That makes it replay-able on BOTH sides with
no shared state:
  - on the native sim  (test/playtest/fake08-sim): spawn each segment, step its frames, check it cleared;
  - on the device       (via harness.py):          deliver the frames frame-synced over serial.
This module is the format + the button-bit mapping shared by every replayer. It is cart-agnostic: each
segment carries an opaque `meta` the cart's adapter understands (for Celeste: the room + spawn).
"""
import json

# button bits — identical on the device (INPUT_* in components/input) and the sim (BIT in fake08sim.py):
#   LEFT=1 RIGHT=2 UP=4 DOWN=8 O(jump,z/o)=16 X(dash,x)=32
BIT = {'l': 1, 'r': 2, 'u': 4, 'd': 8, 'z': 16, 'o': 16, 'x': 32}


def keys_to_mask(keys):
    """'ru' / 'z' / 'rux' / '' -> the OR'd button mask held that frame."""
    m = 0
    for c in keys:
        m |= BIT.get(c, 0)
    return m


def keys_seq_to_masks(plan):
    """A per-frame list of key-strings -> a per-frame list of int masks."""
    return [keys_to_mask(k) for k in plan]


# Inverse: a mask -> the key-chars the serial harness sends (O emitted as 'z', X as 'x'). Order is stable
# so device delivery is reproducible.
_BITS = [(1, 'l'), (2, 'r'), (4, 'u'), (8, 'd'), (16, 'z'), (32, 'x')]


def mask_to_keys(m):
    """A held-button mask -> the key-string the serial input backend expects ('' / 'r' / 'rz' / 'rux')."""
    return ''.join(c for bit, c in _BITS if m & bit)


class Segment:
    """One stretch of the run: `frames` is the per-game-frame held-mask list; `meta` is opaque cart data
    the adapter uses to (a) put the sim at the segment start and (b) detect start/clear on the device."""
    def __init__(self, name, frames, meta=None):
        self.name = name
        self.frames = list(frames)
        self.meta = meta or {}

    def to_dict(self):
        return {"name": self.name, "meta": self.meta, "frames": self.frames}

    @classmethod
    def from_dict(cls, d):
        return cls(d["name"], d["frames"], d.get("meta"))


class Trace:
    """A full replay-able solution: which cart, the game/host frame ratio, and the ordered segments."""
    VERSION = 1

    def __init__(self, cart, segments, steps_per_frame=2, meta=None):
        self.cart = cart                       # cart filename, e.g. "celeste.p8"
        self.segments = list(segments)         # [Segment]
        self.steps_per_frame = steps_per_frame # host Steps per game-frame (2 = a 30 fps cart)
        self.meta = meta or {}                 # free-form (solver, date, fps stats, ...)

    def to_dict(self):
        return {"version": self.VERSION, "cart": self.cart, "steps_per_frame": self.steps_per_frame,
                "meta": self.meta, "segments": [s.to_dict() for s in self.segments]}

    def save(self, path):
        with open(path, "w") as f:
            json.dump(self.to_dict(), f, indent=1)
        return path

    @classmethod
    def load(cls, path):
        with open(path) as f:
            d = json.load(f)
        v = d.get("version")
        if v != cls.VERSION:
            raise ValueError(f"unsupported trace version {v!r} (expected {cls.VERSION}) in {path}")
        return cls(d["cart"], [Segment.from_dict(s) for s in d["segments"]],
                   d.get("steps_per_frame", 2), d.get("meta"))

    def total_frames(self):
        return sum(len(s.frames) for s in self.segments)
