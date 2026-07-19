#!/usr/bin/env python3
"""Faithful-ish port of Celeste Classic player physics + room(0,0) collision (celeste.p8 v36).

Purpose: find a robust button sequence that clears room (0,0) offline, then port to the serial timeline.
Ported 1-to-1 from celeste.p8: player.update (lines 118-336), move/move_x/move_y (1010-1050),
solid_at/tile_flag_at (1393-1414), spikes_at (1416-1432), appr/sign (1374-1387).

Input: per-frame button set. run(seq) returns (trajectory, outcome). outcome in {win, dead-spike, dead-fall, alive}.
"""
import math

# ---- room geometry from cart ----
import os
P8 = os.path.join(os.path.dirname(__file__), "..", "..", "assets", "celeste.p8")
_txt = open(P8).read().splitlines()
def _sec(name):
    out, grab = [], False
    for l in _txt:
        if l.startswith("__"):
            grab = (l.strip() == f"__{name}__"); continue
        if grab: out.append(l)
    return out
_gff = "".join(_sec("gff")); FLAGS = [int(_gff[i:i+2],16) for i in range(0,len(_gff),2)]
while len(FLAGS) < 256: FLAGS.append(0)
_map = _sec("map"); GRID = [[int(r[i:i+2],16) for i in range(0,len(r),2)] for r in _map]
while len(GRID) < 32: GRID.append([0]*128)

RX, RY = 0, 0            # current room; set with set_room()
SPIKE = {17, 27, 43, 59}
SPAWN = (8.0, 96.0)      # player spawn px in the current room (tile 1); set by set_room()
FAKEWALL = None          # solid rect (x0,y0,x1,y1) if a fake_wall (tile 64) is in the room, else None

def fget(t, b): return (FLAGS[t] >> b) & 1
def mget(tx, ty):
    gx, gy = RX*16+tx, RY*16+ty
    if 0 <= tx < 16 and 0 <= ty < 16 and gy < len(GRID): return GRID[gy][gx]
    return 0
def flr(v): return math.floor(v)

def set_room(rx, ry):
    """Point the sim at room (rx,ry): recompute the player spawn (tile 1) and the fake_wall rect (tile 64).
    Only static terrain + spikes + fake_wall are modelled; a room with springs/fall-floors/platforms/etc.
    would need those added before its solution is trustworthy."""
    global RX, RY, SPAWN, FAKEWALL
    RX, RY = rx, ry
    SPAWN, FAKEWALL = (8.0, 96.0), None
    for ty in range(16):
        for tx in range(16):
            t = mget(tx, ty)
            if t == 1:
                SPAWN = (float(tx * 8), float(ty * 8))
            elif t == 64:                       # fake_wall init(tx*8,ty*8), hitbox {x=-1,y=-1,w=18,h=18}
                x0, y0 = tx * 8 - 1, ty * 8 - 1
                FAKEWALL = (x0, y0, x0 + 18, y0 + 18)

set_room(0, 0)           # default room (recompute SPAWN / FAKEWALL for room 0)

def tile_flag_at(x, y, w, h, flag):
    for i in range(max(0, flr(x/8)), min(15, flr((x+w-1)/8)) + 1):
        for j in range(max(0, flr(y/8)), min(15, flr((y+h-1)/8)) + 1):
            if fget(mget(i, j), flag): return True
    return False
def solid_at(x, y, w, h): return tile_flag_at(x, y, w, h, 0)
def ice_at(x, y, w, h): return tile_flag_at(x, y, w, h, 4)

def rect_overlap(x, y, w, h, r):
    return x < r[2] and x+w > r[0] and y < r[3] and y+h > r[1]

def spikes_at(x, y, w, h, xspd, yspd):
    for i in range(max(0, flr(x/8)), min(15, flr((x+w-1)/8)) + 1):
        for j in range(max(0, flr(y/8)), min(15, flr((y+h-1)/8)) + 1):
            t = mget(i, j)
            if t == 17 and ((y+h-1) % 8 >= 6 or y+h == j*8+8) and yspd >= 0: return True
            if t == 27 and y % 8 <= 2 and yspd <= 0: return True
            if t == 43 and x % 8 <= 2 and xspd <= 0: return True
            if t == 59 and ((x+w-1) % 8 >= 6 or x+w == i*8+8) and xspd >= 0: return True
    return False

def appr(val, target, amount):
    return max(val-amount, target) if val > target else min(val+amount, target)
def sign(v): return 1 if v > 0 else (-1 if v < 0 else 0)

MAXRUN, MAX_DJUMP = 1, 1
HB = dict(x=1, y=3, w=6, h=5)
MOVE_INCLUSIVE = True     # replicate cart's `for i=0,abs(amount)` (inclusive). Calibrated below.

class Player:
    def __init__(self):
        self.x, self.y = SPAWN
        self.rem_x = self.rem_y = 0.0
        self.spd_x = self.spd_y = 0.0
        self.p_jump = self.p_dash = False
        self.grace = 0; self.jbuffer = 0
        self.djump = MAX_DJUMP
        self.dash_time = 0; self.dash_effect_time = 0
        self.dash_target_x = self.dash_target_y = 0.0
        self.dash_accel_x = self.dash_accel_y = 0.0
        self.flip_x = False
        self.dashed_this_frame = False

    def clone(self):
        q = Player.__new__(Player)
        q.__dict__.update(self.__dict__)
        return q

    def is_solid(self, ox, oy):
        hx, hy, hw, hh = HB['x'], HB['y'], HB['w'], HB['h']
        if solid_at(self.x+hx+ox, self.y+hy+oy, hw, hh): return True
        if FAKEWALL is not None and rect_overlap(self.x+hx+ox, self.y+hy+oy, hw, hh, FAKEWALL): return True
        return False
    def is_ice(self, ox, oy):
        return ice_at(self.x+HB['x']+ox, self.y+HB['y']+oy, HB['w'], HB['h'])

    def move(self, ox, oy):
        self.rem_x += ox
        amt = flr(self.rem_x + 0.5); self.rem_x -= amt; self._move_x(amt, 0)
        self.rem_y += oy
        amt = flr(self.rem_y + 0.5); self.rem_y -= amt; self._move_y(amt)
    def _move_x(self, amount, start):
        step = sign(amount)
        end = abs(amount) + (1 if MOVE_INCLUSIVE else 0)
        for _ in range(start, end):
            if not self.is_solid(step, 0): self.x += step
            else: self.spd_x = 0; self.rem_x = 0; break
    def _move_y(self, amount):
        step = sign(amount)
        end = abs(amount) + (1 if MOVE_INCLUSIVE else 0)
        for _ in range(0, end):
            if not self.is_solid(0, step): self.y += step
            else: self.spd_y = 0; self.rem_y = 0; break

    def update(self, btn):
        # btn: dict with keys L,R,U,D,O(jump),X(dash) -> bool
        L, R, U, D = btn.get('L'), btn.get('R'), btn.get('U'), btn.get('D')
        JMP, DSH = btn.get('O'), btn.get('X')
        inp = 1 if R else (-1 if L else 0)
        # spikes / bottom death checked by caller after move; here just movement
        on_ground = self.is_solid(0, 1)
        on_ice = self.is_ice(0, 1)
        jump = JMP and not self.p_jump
        self.p_jump = JMP
        if jump: self.jbuffer = 4
        elif self.jbuffer > 0: self.jbuffer -= 1
        dash = DSH and not self.p_dash
        self.p_dash = DSH
        if on_ground:
            self.grace = 6
            if self.djump < MAX_DJUMP: self.djump = MAX_DJUMP
        elif self.grace > 0: self.grace -= 1
        self.dash_effect_time -= 1
        if self.dash_time > 0:
            self.dash_time -= 1
            self.spd_x = appr(self.spd_x, self.dash_target_x, self.dash_accel_x)
            self.spd_y = appr(self.spd_y, self.dash_target_y, self.dash_accel_y)
        else:
            maxrun, accel, deccel = 1, 0.6, 0.15
            if not on_ground: accel = 0.4
            elif on_ice:
                accel = 0.05
                if inp == (-1 if self.flip_x else 1): accel = 0.05
            if abs(self.spd_x) > maxrun:
                self.spd_x = appr(self.spd_x, sign(self.spd_x)*maxrun, deccel)
            else:
                self.spd_x = appr(self.spd_x, inp*maxrun, accel)
            if self.spd_x != 0: self.flip_x = (self.spd_x < 0)
            maxfall, gravity = 2, 0.21
            if abs(self.spd_y) <= 0.15: gravity *= 0.5
            if inp != 0 and self.is_solid(inp, 0) and not self.is_ice(inp, 0):
                maxfall = 0.4
            if not on_ground:
                self.spd_y = appr(self.spd_y, maxfall, gravity)
            # jump
            if self.jbuffer > 0:
                if self.grace > 0:
                    self.jbuffer = 0; self.grace = 0; self.spd_y = -2
                else:
                    wall_dir = -1 if self.is_solid(-3, 0) else (1 if self.is_solid(3, 0) else 0)
                    if wall_dir != 0:
                        self.jbuffer = 0; self.spd_y = -2
                        self.spd_x = -wall_dir*(maxrun+1)
            # dash
            d_full = 5; d_half = d_full*0.70710678118
            if self.djump > 0 and dash:
                self.dashed_this_frame = True     # triggers freeze=2 in the run loop
                self.djump -= 1
                self.dash_time = 4
                self.dash_effect_time = 10
                v_input = -1 if U else (1 if D else 0)
                if inp != 0:
                    if v_input != 0:
                        self.spd_x = inp*d_half; self.spd_y = v_input*d_half
                    else:
                        self.spd_x = inp*d_full; self.spd_y = 0
                elif v_input != 0:
                    self.spd_x = 0; self.spd_y = v_input*d_full
                else:
                    self.spd_x = (-1 if self.flip_x else 1); self.spd_y = 0
                self.dash_target_x = 2*sign(self.spd_x)
                self.dash_target_y = 2*sign(self.spd_y)
                self.dash_accel_x = 1.5; self.dash_accel_y = 1.5
                if self.spd_y < 0: self.dash_target_y *= 0.75
                if self.spd_y != 0: self.dash_accel_x *= 0.70710678118
                if self.spd_x != 0: self.dash_accel_y *= 0.70710678118
            elif dash and self.djump <= 0:
                pass
        # NOTE: move() is intentionally NOT called here. Celeste's _update does obj.move(spd) BEFORE
        # obj.type.update(obj), so movement uses the PREVIOUS frame's spd. The run loop calls move first.


# --- serial input model (matches input_serial.c: each byte sets that button's hold to HOLD; input_poll
#     called once per game-update decrements). key->PICO8 button. Lets the search produce a route that is
#     realizable over the real serial backend and delivered frame-synced. ---
KEY2BTN = {'l': 'L', 'r': 'R', 'u': 'U', 'd': 'D', 'z': 'O', 'o': 'O', 'x': 'X'}

def events_to_seq(events, nframes, hold=6):
    """events: list of (frame, key). Returns per-frame [button-dict] using input_serial semantics."""
    byframe = {}
    for f, k in events:
        byframe.setdefault(f, []).append(k)
    holdc = {}
    seq = []
    for f in range(nframes):
        for k in byframe.get(f, []):
            b = KEY2BTN.get(k)
            if b:
                holdc[b] = hold
        btn = {}
        for b in list(holdc):
            if holdc[b] > 0:
                btn[b] = True
                holdc[b] -= 1
        seq.append(btn)
    return seq

def run_events(events, nframes=260, hold=6):
    return run(events_to_seq(events, nframes, hold), nframes)


def simulate(active_seq, maxframes=400):
    """active_seq: per-ACTIVE-frame button-dicts (no freeze frames). Models Celeste's real ordering:
    obj.move(spd) runs BEFORE obj.type.update, and a dash sets freeze=2 (2 fully-skipped frames).
    Returns (traj, outcome, game_plan). traj + game_plan are indexed by GAME-frame (incl. freeze) so they
    line up 1:1 with the hardware frame counter for frame-synced delivery."""
    p = Player()
    traj = []
    game_plan = []          # per game-frame buttons actually applied (held-previous during freeze)
    freeze = 0
    ai = 0
    last_btn = {}
    for gf in range(maxframes):
        if freeze > 0:
            freeze -= 1
            traj.append((gf, p.x, p.y, 'freeze'))
            game_plan.append(dict(last_btn))
            continue
        btn = active_seq[ai] if ai < len(active_seq) else {}
        ai += 1
        last_btn = btn
        game_plan.append(dict(btn))
        p.move(p.spd_x, p.spd_y)                    # move with previous spd (Celeste order)
        if spikes_at(p.x+HB['x'], p.y+HB['y'], HB['w'], HB['h'], p.spd_x, p.spd_y):
            traj.append((gf, p.x, p.y, 'spike')); return traj, 'dead-spike', game_plan
        if p.y > 128:
            traj.append((gf, p.x, p.y, 'fall')); return traj, 'dead-fall', game_plan
        p.dashed_this_frame = False
        p.update(btn)                                # compute next spd + jump/dash (no move inside)
        traj.append((gf, p.x, p.y, ''))
        if p.y < -4:
            return traj, 'win', game_plan
        if p.dashed_this_frame:
            freeze = 2
    return traj, 'alive', game_plan

def run(seq, maxframes=400):
    traj, outcome, _ = simulate(seq, maxframes)
    return traj, outcome


if __name__ == "__main__":
    # smoke: stand still 5 frames
    t, o = run([{}]*10)
    print("still:", o, "end", round(t[-1][1],1), round(t[-1][2],1))
