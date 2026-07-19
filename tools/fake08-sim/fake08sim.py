"""ctypes bindings for libfake08sim.so — drive the real fake-08 VM (the device's VM) headlessly.

    import fake08sim as S
    S.init("assets/celeste.p8"); S.start_room(0,0)
    S.save()                       # snapshot the spawn
    for keys in plan: S.step(keys)  # keys like 'ru' / 'z' / '' (held that game-frame)
    st = S.read()                  # {found,x,y,rx,ry,dj}
    S.restore()                    # back to the snapshot (for the next search candidate)
"""
import ctypes, os
_lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), "libfake08sim.so"))
_lib.sim_init.argtypes = [ctypes.c_char_p]; _lib.sim_init.restype = ctypes.c_int
_lib.sim_start_room.argtypes = [ctypes.c_int, ctypes.c_int]
_lib.sim_step.argtypes = [ctypes.c_ubyte]
_lib.sim_read.argtypes = [ctypes.POINTER(ctypes.c_int)] * 9
_lib.sim_frame_rgb.argtypes = [ctypes.POINTER(ctypes.c_ubyte)]

# serial keys -> PICO-8 button bits (P8_KEY_*): L=1 R=2 U=4 D=8 O(z/o)=16 X(x)=32
BIT = {'l': 1, 'r': 2, 'u': 4, 'd': 8, 'z': 16, 'o': 16, 'x': 32}

def mask(keys):
    m = 0
    for c in keys:
        m |= BIT.get(c, 0)
    return m

def init(cart):
    if not _lib.sim_init(cart.encode()):
        raise RuntimeError("sim_init failed: " + cart)

def start_room(rx, ry): _lib.sim_start_room(rx, ry)
def step(keys):         _lib.sim_step(mask(keys))
def save():             _lib.sim_save()
def restore():          _lib.sim_restore()

def spawn(rx, ry, maxsteps=80):
    """begin_game() + load_room(rx,ry), then step until the player_spawn animation finishes and the real
    player exists. Returns its state (at the room's spawn)."""
    start_room(rx, ry)
    for _ in range(maxsteps):
        step('')
        st = read()
        if st['found']:
            return st
    raise RuntimeError(f"player never spawned in room ({rx},{ry})")

def replay(plan, maxextra=40):
    """Play a per-game-frame key plan; return (cleared, miny, states). cleared = room advanced."""
    start = read(); sr = (start['rx'], start['ry'])
    miny = start['y']; cleared = False
    for keys in plan:
        step(keys)
        st = read()
        miny = min(miny, st['y'])
        if (st['rx'], st['ry']) != sr:
            cleared = True; break
    return cleared, miny

def read():
    v = [ctypes.c_int() for _ in range(9)]
    _lib.sim_read(*[ctypes.byref(x) for x in v])
    f, x, y, rx, ry, dj, fz, sx, sy = [c.value for c in v]
    return dict(found=f, x=x, y=y, rx=rx, ry=ry, dj=dj, fz=fz, sx=sx, sy=sy)

def frame_rgb():
    buf = (ctypes.c_ubyte * (128 * 128 * 3))()
    _lib.sim_frame_rgb(buf)
    return bytes(buf)
