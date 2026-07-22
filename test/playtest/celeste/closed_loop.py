#!/usr/bin/env python3
"""Celeste room (0,0) — a LIVE CLOSED-LOOP policy (the feedback counterpart to the open-loop trace in
celeste_playtest.py / solution.trace.json). A `policy(state) -> mask` reads the player state each frame and
reacts, run through ../live.py's drive_sim, exactly like pico_racer/ does for the racer.

Room (0,0) = "100 M" clears in 90 frames on the SIM (zero latency), deterministically. The climb is a
reactive phase machine: each dash/jump fires on a STATE predicate (grounded / rising-into-a-launch-window /
djump-refreshed), never a frame index. Two techniques crack the pixel-precise air-dashes:
  * STEER-TO-LAUNCH — drive x to the dash launch point as the player rises, then dash at the apex, so the
    launch is robust to upstream drift instead of depending on a byte-exact arc; and
  * COYOTE-GRACE jumps off the short ledges (jump while grace>0, not only while grounded).
The few launch thresholds in PARAMS were tuned on the sim (like the racer's PARAMS), not hand-guessed.

    python3 test/playtest/celeste/closed_loop.py            # run the policy live on the sim, report the clear
    python3 test/playtest/celeste/closed_loop.py --film out.png   # + a filmstrip of the climb

DEVICE NOTE: closed-loop-over-serial is NOT viable for this room — the board's ~1-frame JITTERY control
latency is fatal to the pixel-exact air-dashes (measured: 0-frame lag clears, any jitter fails; see
../fc_latency.py). The device path needs the fc-scheduled input backend (docs/runtime/pico-e32-fake08-input.md);
until then the room ships open-loop (celeste_playtest.py). This module is the sim-side closed-loop solve."""
import os, sys, ctypes, json

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))                 # live.py
sys.path.insert(0, os.path.join(HERE, "..", "fake08-sim"))   # fake08sim
import fake08sim as VM

CART = os.path.join(REPO, "assets", "celeste.p8")
L, R, U, D, O, X = 1, 2, 4, 8, 16, 32
ROOM = (0, 0)

# The observation TAIL: a Lua expression (racer-style) that pokes the reactive state into scratch RAM 0x4300+
# (Celeste doesn't touch it), read back via sim_peek. The SAME fields the device telemetry would stream: it
# adds on_ground / full spd / grace / wall-contact / dash_time to the plain x/y/room/spd/djump, which the
# reactive policy needs. Leaves the shared sim.cpp reader untouched.
TAIL = (
    "poke(0x4300,0) local p for o in all(objects) do if o.type==player then p=o end end "
    "if p then poke(0x4300,1) poke(0x4301,p.x) poke(0x4302,p.y+64) poke(0x4305,p.djump) "
    "poke(0x4307,p.spd.x*16+128) poke(0x4308,p.spd.y*16+128) "
    "poke(0x4309,p.is_solid(0,1) and 1 or 0) poke(0x430a,p.grace) "
    "poke(0x430b,(p.is_solid(-3,0) and 1 or 0)+(p.is_solid(3,0) and 2 or 0)) "
    "poke(0x430c,p.dash_time) end "
    "poke(0x4303,room.x) poke(0x4304,room.y) poke(0x4306,freeze or 0)"
).encode()

_peek_ready = False
def _read():
    """SIM state source — the same dict shape the device telemetry would parse. Runs TAIL in the cart sandbox
    and reads scratch RAM 0x4300+ via sim_peek (no shared-core change)."""
    global _peek_ready
    if not _peek_ready:
        VM._lib.sim_peek.argtypes = [ctypes.c_int]; VM._lib.sim_peek.restype = ctypes.c_int
        _peek_ready = True
    VM._lib.sim_exec(TAIL)
    d = {a: VM._lib.sim_peek(a) for a in range(0x4300, 0x430d)}
    return dict(found=d[0x4300], x=d[0x4301], y=d[0x4302] - 64, rx=d[0x4303], ry=d[0x4304],
                dj=d[0x4305], fz=d[0x4306], spx=(d[0x4307] - 128) / 16.0, spy=(d[0x4308] - 128) / 16.0,
                grnd=d[0x4309], grace=d[0x430a], wall=d[0x430b], dasht=d[0x430c], fc=VM.frame_count())

read = _read   # the `read` callable live.drive_sim expects

# Reactive launch/timing thresholds — state predicates, tuned on the sim (the closed-loop analogue of PARAMS).
PARAMS = dict(
    ul_coast_x=63,                    # ph1: stop pushing Right once x>=this (coast into the apex)
    ul_dash_xlo=62, ul_dash_xhi=68,   # ph1: fire dash-UL when x in this band ...
    ul_dash_spy=-0.4, ul_dash_yhi=66, # ... near the apex (spy>=this) and y<=yhi, dj available
    p3_jump_x=42,                     # ph3: jump (coyote grace ok) once walked right to x>=this
    ur_dash_xlo=49, ur_dash_xhi=54,   # ph4: fire dash-UR band
    ur_dash_yhi=32,
)


class Climber:
    """policy(state)->mask for room (0,0). Ordered phases; each dash/jump fires on a STATE predicate and
    advances on observed state (grounded / rising-to-a-launch-window / djump refreshed) — never a frame
    index, so it is robust to timing drift. Air-dashes STEER to their launch point. Instantiate fresh per
    run; `reset()` between runs."""

    def __init__(self, params=None):
        self.P = dict(PARAMS, **(params or {}))
        self.reset()

    def reset(self):
        self.ph = 0; self.tk = 0; self.prevO = False; self.prevX = False; self.acted = False

    def _to(self, ph):
        self.ph, self.tk, self.acted = ph, 0, False

    def _ride(self, hdir, st):
        return hdir | (U if (st['dasht'] > 0 or st['spy'] <= -1.2) else 0)

    def __call__(self, st):
        self.tk += 1
        P = self.P
        m = 0
        x, y, dj = st['x'], st['y'], st['dj']
        canjump = st['grnd'] or st['grace'] > 0 or st['wall'] != 0
        if self.ph == 0:                  # DASH UR from spawn, ride, land on the mid ledge
            m = (R | U | X) if self.tk == 1 else self._ride(R, st)
            if st['grnd'] and self.tk > 5:
                self._to(1)
        elif self.ph == 1:                # JUMP, coast to the apex at x~launch, DASH UL
            if not self.acted:
                m = (R | O) if canjump else R
                if canjump:
                    self.acted = True
            elif (dj >= 1 and P['ul_dash_xlo'] <= x <= P['ul_dash_xhi']
                  and y <= P['ul_dash_yhi'] and st['spy'] >= P['ul_dash_spy']):
                m = L | U | X; self._to(2)
            else:
                m = R if x < P['ul_coast_x'] else 0     # push right, then coast into the apex
        elif self.ph == 2:                # ride DASH UL, redirect right, land on the x~35 ledge
            m = (L | U) if (st['dasht'] > 0 or st['spy'] <= -1.2) else R
            if st['grnd'] and self.tk > 5:
                self._to(3)
        elif self.ph == 3:                # walk right, JUMP at x>=thresh (coyote grace ok — ledge ends ~x40)
            if canjump and x >= P['p3_jump_x']:
                m = R | O; self._to(4)
            else:
                m = R
        elif self.ph == 4:                # rise from jump, DASH UR at the launch band
            if dj >= 1 and P['ur_dash_xlo'] <= x <= P['ur_dash_xhi'] and y <= P['ur_dash_yhi']:
                m = R | U | X; self._to(5)
            else:
                m = R
        elif self.ph == 5:                # ride DASH UR toward the top
            m = self._ride(R, st)
            if y <= 18:
                self._to(6)
        else:                             # TOP: hold right, hop over the final lips (ground + wall jumps)
            m = R
            if canjump and not self.prevO:
                m |= O
        # edge O and X: if pressed last frame, release now so the next press is a clean 0->1 edge
        if (m & O) and self.prevO: m &= ~O
        if (m & X) and self.prevX: m &= ~X
        self.prevO = bool(m & O); self.prevX = bool(m & X)
        return m


# ---- Room (1,0) = "200 M": a vertical climb — left channel, launch + cross-dash, right shaft ----
# Same reactive-phase style as the room-(0,0) Climber (each move fires on a STATE predicate, never a frame
# index). The room map: a LEFT channel (cols 4-6) between the col-3 wall and the col-7 pillar; a gap (cols
# 8-9) past the pillar; a RIGHT shaft (cols 8-12) to the exit (top, cols 12-14). The one air-dash crosses
# the gap; the rest is wall-jumps. Thresholds tuned on the sim.
PARAMS200 = dict(
    approach_x=30,    # walk right to the channel base before climbing
    climb_top_y=50,   # climb the pillar until y<=this, then set up the launch
    jump_spy=-0.8,    # one wall/ground jump per contact (don't re-jump while already rising)
    dash_x=70, dash_ylo=40, dash_yhi=54,   # coast right, dash past the pillar at pillar-top-ledge height
)


class Climber200:
    """policy(state)->mask for room (1,0). Phases fire on observed state: APPROACH the channel, wall-jump
    CLIMB the left channel, LAUNCH off the left wall (-> rightward momentum), COAST right + cross-DASH over
    the pillar into the right shaft, then RCLIMB the right wall to the top-right exit. Fresh per run."""
    PHN = {0: "approach", 1: "climb", 2: "launch", 3: "coast", 4: "rclimb"}

    def __init__(self, params=None):
        self.P = dict(PARAMS200, **(params or {})); self.reset()

    def reset(self):
        self.ph = 0; self.tk = 0; self.prevO = False; self.prevX = False; self.spawned = False

    def _jump(self, st):        # a jump is possible + a clean 0->1 edge + not already rising fast
        return (st['grnd'] or st['grace'] > 0 or st['wall'] != 0) and not self.prevO and st['spy'] >= self.P['jump_spy']

    def __call__(self, st):
        self.tk += 1; P = self.P
        x, y, dj, spy, wall = st['x'], st['y'], st['dj'], st['spy'], st['wall']
        m = 0
        if not self.spawned:                               # entering the room (0,0)->(1,0) transition drops the
            if st['grnd'] and y >= 100:                    # player in at the top then respawns it at the bottom;
                self.spawned = True                        # hold neutral until it settles on the spawn floor,
            return 0                                        # so the phase machine starts from a clean spawn
        if self.ph == 0:                                   # APPROACH: walk right to the channel base
            m = R
            if x >= P['approach_x']:
                self.ph, self.tk = 1, 0
        elif self.ph == 1:                                 # CLIMB: hug the pillar, wall-jump up the channel
            m = R
            if self._jump(st):
                m |= O
            if y <= P['climb_top_y'] and self.tk > 3:
                self.ph, self.tk = 2, 0
        elif self.ph == 2:                                 # LAUNCH: drift to the LEFT wall, jump off it (-> right)
            m = L
            if wall == 1 and not self.prevO:
                m = L | O; self.ph, self.tk = 3, 0
            elif self.tk > 20:                             # safety: never reached the wall -> try to cross anyway
                self.ph, self.tk = 3, 0
        elif self.ph == 3:                                 # COAST: ride the launch right over the pillar, then DASH
            m = R
            if dj >= 1 and (P['dash_x'] <= x and P['dash_ylo'] <= y <= P['dash_yhi']
                            or y > P['dash_yhi'] + 12):     # in the launch band, or falling past it -> cross now
                m = R | U | X; self.ph, self.tk = 4, 0
        else:                                              # RCLIMB: arc right to the right wall, wall-jump up
            m = R                                          # hold RIGHT (no up-ride) so the dash arcs right + settles
            if wall == 2 and self._jump(st):               # only wall-jump off the RIGHT wall -> climbs the exit shaft
                m |= O
        if (m & O) and self.prevO: m &= ~O
        if (m & X) and self.prevX: m &= ~X
        self.prevO = bool(m & O); self.prevX = bool(m & X)
        return m


# ---- Room (2,0) = "300 M": spring + dash + wall-jump climb, past two spike fields to the top-right exit ----
# The exit sits above the upper spikes, so the only clean approach is the right shaft (col 11). No open-loop
# reference exists and the beam-search twin can't help (it doesn't model springs), so the route was found by a
# beam search on the REAL VM (full physics) and reverse-engineered here: dash R into spring1, ride the BOUNCE
# and DASH UR at the apex to the right wall, two wall-jumps up, DASH UP into the exit gap, wall-jump through.
PARAMS300 = dict(
    ur_y=78, ur_spy=0.0,     # dash UR at the spring-bounce apex (spy>=this, y<=this), dash available
    wj1_x=106,               # first wall-jump L once the UR dash flies to the right wall (x>=this)
    du_x=95, du_y=44,        # dash UP only once drifted into the exit gap (x<=this, cols 8-11), y<=this
    top_y=16,                # wall-jump R near the top (y<=this) on wall contact -> punch through the exit
)


class Climber300:
    """policy(state)->mask for room (2,0). Phases fire on observed state: DASH R into spring1, ride the BOUNCE
    and DASH UR at the apex, fly to the right wall and WALL-JUMP up, DASH UP into the exit gap, wall-jump
    through. Uses the spring the beam twin couldn't model. Fresh per run; `spawned` gates the chain-entry."""
    PHN = {0: "dashR", 1: "ride+apexUR", 3: "fly->wj", 4: "climb->dashU", 5: "top"}

    def __init__(self, params=None):
        self.P = dict(PARAMS300, **(params or {})); self.reset()

    def reset(self):
        self.ph = 0; self.tk = 0; self.prevO = False; self.prevX = False
        self.spawned = False; self.bounced = False; self.wj = 0

    def __call__(self, st):
        self.tk += 1; Q = self.P
        x, y, dj, spy, wall = st['x'], st['y'], st['dj'], st['spy'], st['wall']
        if not self.spawned:                         # chain-entry gate: settle on the spawn floor first
            if st['grnd'] and y >= 100:
                self.spawned = True
            else:
                return 0
        m = 0
        if self.ph == 0:                             # DASH RIGHT from spawn (carry speed into spring1)
            m = R | X; self.ph, self.tk = 1, 0
        elif self.ph == 1:                           # ride onto spring1, BOUNCE, DASH UR at the apex
            m = R
            if spy < -2:
                self.bounced = True
            if self.bounced and dj >= 1 and spy >= Q['ur_spy'] and y <= Q['ur_y']:
                m = R | U | X; self.ph, self.tk = 3, 0
        elif self.ph == 3:                           # ride the UR dash to the right wall, wall-jump LEFT
            m = R
            if x >= Q['wj1_x']:
                m = L | O; self.ph, self.tk = 4, 0
        elif self.ph == 4:                           # one more wall-jump, then drift LEFT into the gap + DASH UP
            m = L
            if self.wj < 1 and wall != 0 and not self.prevO:
                m = L | O; self.wj += 1
            elif dj >= 1 and x <= Q['du_x'] and y <= Q['du_y']:
                m = U | X; self.ph, self.tk = 5, 0
        else:                                        # ride the up-dash, wall-jump RIGHT to punch through the exit
            m = U if (st['dasht'] > 0 or spy <= -1.4) else R
            if wall != 0 and not self.prevO and y <= Q['top_y']:
                m = R | O
        if (m & O) and self.prevO: m &= ~O
        if (m & X) and self.prevX: m &= ~X
        self.prevO = bool(m & O); self.prevX = bool(m & X)
        return m


# Room registry: room coord -> (name, spawn, policy factory, death-y). "room by room" lives here.
class ReplayClimber:
    """policy(state)->mask for a room solved as a RAW MASK SEQUENCE (e.g. 400 M, whose route is a pixel-precise
    wall-jump chain that isn't easily reactive). A `spawned` gate holds neutral until the player is grounded at
    the room spawn (surviving the room-entry transition when chained), then replays the masks frame-by-frame.
    Instantiated per run; `reset()` between runs. The masks come from a solved+VM-verified route JSON."""

    def __init__(self, masks=None, spawn=(8, 96), params=None):
        self.masks = list(masks or [])
        self.spawn = spawn
        self.reset()

    def reset(self):
        self.i = 0
        self.spawned = False

    def __call__(self, st):
        if not self.spawned:
            if st['grnd'] and abs(st['x'] - self.spawn[0]) < 8 and abs(st['y'] - self.spawn[1]) < 8:
                self.spawned = True
            else:
                return 0
        m = self.masks[self.i] if self.i < len(self.masks) else 0
        self.i += 1
        return m


def _load_route(name):
    p = os.path.join(HERE, "routes", name)
    return json.load(open(p))["masks"] if os.path.exists(p) else []


# 400 M route: the VM-verified wall-jump chain (route B — lands on D, matches the drawn route). Raw masks.
ROUTE_400 = _load_route("room400_altroute_viaD.json")


def make_replay(masks, spawn):
    """factory usable as ROOMS[rc][2]: callable with no args (Chain) or a params arg (drive_sim_room)."""
    return lambda params=None: ReplayClimber(masks, spawn)


ROOMS = {
    (0, 0): ("100 M", (8.0, 96.0), Climber, 122),
    (1, 0): ("200 M", (8.0, 112.0), Climber200, 130),
    (2, 0): ("300 M", (8.0, 104.0), Climber300, 128),
    (3, 0): ("400 M", (8.0, 96.0), make_replay(ROUTE_400, (8, 96)), 128),
}


class Chain:
    """policy(state)->mask that plays a SEQUENCE of rooms, switching to the right per-room policy on the live
    room. This is the whole-run controller the device twin-in-the-loop drives: room (0,0) -> (1,0) -> ... ,
    each cleared closed-loop, since the board can only reach room N by clearing room N-1."""

    def __init__(self, rooms=((0, 0), (1, 0))):
        self.rooms = tuple(rooms); self.reset()

    def reset(self):
        self.pols = {rc: ROOMS[rc][2]() for rc in self.rooms}

    def __call__(self, st):
        rc = (int(st['rx']), int(st['ry']))
        p = self.pols.get(rc)
        return p(st) if p is not None else R      # off-plan room: hold right (harmless until is_done fires)


def is_done(st):
    """Run over: the room advanced (cleared) or the player fell out the bottom (dead)."""
    return (st['rx'], st['ry']) != ROOM or st['y'] > 122


def room_done(room, deathy=130):
    """is_done for a single room in isolation: advanced out of `room`, or fell out the bottom."""
    return lambda st: (st['rx'], st['ry']) != room or st['y'] > deathy


def chain_done(rooms):
    """is_done for a chained run: reached the room AFTER the last in `rooms` (all cleared), or fell out."""
    last = rooms[-1]; after = (float(last[0] + 1), float(last[1]))
    ymax = max(ROOMS[rc][3] for rc in rooms)
    return lambda st: (st['rx'], st['ry']) == after or st['y'] > ymax


def drive_sim_room(room, params=None, record=False, verbose=True, max_frames=280):
    """Run one room's policy in isolation on the sim (VM.spawn(room)); mirrors drive_sim for any room."""
    import live
    name, spawn, Pol, deathy = ROOMS[room]
    pol = Pol(params)
    done = room_done(room, deathy)
    r = live.drive_sim(CART, pol, read, done, reset=lambda _: (pol.reset(), VM.spawn(*room)),
                       max_frames=max_frames, record=record)
    fin = r["final"]; cleared = (fin["rx"], fin["ry"]) != room
    if verbose:
        print(f"closed-loop room {room} ({name}): {r['count']} frames -> room=({fin['rx']},{fin['ry']}) "
              f"{'CLEAR' if cleared else 'FAIL'}")
    return dict(cleared=cleared, frames=r["count"], final=fin, masks=r["frames"])


def drive_sim_chain(rooms=((0, 0), (1, 0)), record=False, verbose=True, max_frames=520):
    """Play a chain of rooms end to end on the sim (spawn the first, let the game advance): the device
    scenario, since the board reaches room N only by clearing N-1. Returns the same dict shape as drive_sim."""
    import live
    pol = Chain(rooms)
    done = chain_done(rooms)
    r = live.drive_sim(CART, pol, read, done, reset=lambda _: (pol.reset(), VM.spawn(*rooms[0])),
                       max_frames=max_frames, record=record)
    fin = r["final"]; after = (float(rooms[-1][0] + 1), float(rooms[-1][1]))
    cleared = (fin["rx"], fin["ry"]) == after
    if verbose:
        print(f"closed-loop chain {list(rooms)}: {r['count']} frames -> room=({fin['rx']},{fin['ry']}) "
              f"{'CLEAR all' if cleared else 'STOPPED'}")
    return dict(cleared=cleared, frames=r["count"], final=fin, masks=r["frames"])


def reset_for(pol):
    """live.drive_sim `reset` hook: reset the policy and load room (0,0) (drive_sim boots to the title first)."""
    def r(_):
        pol.reset(); VM.spawn(*ROOM)
    return r


def drive_sim(params=None, record=False, verbose=True):
    """Run the policy live on the sim via live.drive_sim -> dict(cleared, frames, final, masks). `record`
    also captures the per-frame masks (an open-loop Trace segment for the device/render path)."""
    import live
    pol = Climber(params)
    r = live.drive_sim(CART, pol, read, is_done, reset=reset_for(pol), max_frames=220, record=record)
    fin = r["final"]
    cleared = (fin["rx"], fin["ry"]) != ROOM
    if verbose:
        print(f"closed-loop room {ROOM}: {r['count']} frames -> room=({fin['rx']},{fin['ry']}) "
              f"{'CLEAR' if cleared else 'FAIL'}")
    return dict(cleared=cleared, frames=r["count"], final=fin, masks=r["frames"])


def main():
    film = None
    if "--film" in sys.argv:
        i = sys.argv.index("--film")
        film = sys.argv[i + 1] if i + 1 < len(sys.argv) else "closed_loop.png"
    r = drive_sim(record=film is not None)
    if film and r["masks"]:
        import gym
        gym.run_filmstrip(r["masks"], film, every=4, cols=8, scale=2,
                          reset=lambda: VM.spawn(*ROOM), label=lambda s, i: f"f{i} y{s['y']}")
        print(f"wrote {film}")
    return 0 if r["cleared"] else 1


if __name__ == "__main__":
    sys.exit(main())
