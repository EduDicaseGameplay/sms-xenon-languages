# Run under gdb with a fresh memory card and SMS_WARP=3,7,1 (Ricco Harbor),
# headless. See docs/DEVELOPMENT.md for the invocation.
# The F9 menu's Low Gravity and Infinite Air (decomp-patches/port-12-cheat-
# infinite-air.patch, port-13-cheat-low-gravity.patch). Low Gravity: a jump
# from standing, A held (the left click, through the pad layer's key state)
# until he lands, at each setting; the rise must go with 1 / gravity. Infinite
# Air: Mario is dropped into the nearest deep sea the stage's collision has,
# then held DEPTH under its surface for ten seconds, with the cheat off and
# on; the meter must go down without it and stay full with it. No game code
# is skipped; the air check moves Mario.
import time

import gdb

PAD = "'(anonymous namespace)::%s'"
M = 'gpMarioOriginal'
MOUSE_LEFT = 513  # A in the default bindings
AIRBORNE = 0x800
IN_WATER, GAME_OVER = 0x20000, 0x400  # MARIO_FLAG_*
DEPTH = 300.0     # how far under the surface Mario is held
# BG_TYPE_WATER, _SEA_WATER and _POOL without property flags; BG_TYPE_DEATH_PLANE
WATER, DEATH = (0x100, 0x102, 0x104), 0x800

# name, setting, value
CHECKS = [
    ('jump, gravity 1', 'port_cheat_low_gravity', 1),
    ('jump, gravity 1/2', 'port_cheat_low_gravity', 2),
    ('jump, gravity 1/4', 'port_cheat_low_gravity', 4),
    ('under water, air off', 'port_cheat_infinite_air', 0),
    ('under water, air on', 'port_cheat_infinite_air', 1),
]


def ev(e):
    return gdb.parse_and_eval(e)


def setv(name, value):
    gdb.execute('set var %s = %s' % (name, value), to_string=True)


def key(code, down):
    setv(PAD % 'g_key' + '[%d]' % code, 1 if down else 0)


def fail(message):
    print('cheats: error: ' + message, flush=True)
    gdb.execute('kill')
    gdb.execute('quit 1')


def ground(fn, x, y, z):
    # a TMap ground check, its plane stored in Mario's (set again each frame)
    h = float(ev('gpMap->%s(%.1ff, %.1ff, %.1ff, (const TBGCheckData**)&%s->mWaterFloor)' % (fn, x, y, z, M)))
    plane = int(ev(M + '->mWaterFloor'))
    kind = int(ev(M + '->mWaterFloor->mBGType')) if plane else -1
    return h, kind


def find_water(x0, z0):
    # the nearest point, on a 500-unit grid within 4000 units, of water with
    # a floor (not a death plane) at least DEPTH + 300 under its surface
    best = None
    for i in range(-8, 9):
        for j in range(-8, 9):
            x, z = x0 + 500 * i, z0 + 500 * j
            top, kind = ground('checkGround', x, 20000.0, z)
            if kind < 0 or kind & 0xFFF not in WATER:
                continue
            bottom, kind = ground('checkGroundIgnoreWaterSurface', x, top - 1.0, z)
            if kind < 0 or kind & 0xFFF == DEATH or top - bottom < DEPTH + 300:
                continue
            d = i * i + j * j
            if best is None or d < best[0]:
                best = (d, x, z, top)
    return best and best[1:]


class NormalCamera(gdb.Breakpoint):
    hits = 0

    def stop(self):
        self.hits += 1
        return False


class Frames(gdb.Breakpoint):
    def __init__(self):
        super().__init__('TMarDirector::direct')
        self.rate = 1
        self.settled = 0
        self.normal_hits = 0
        self.waited = 0
        self.check = 0
        self.phase = 'land'
        self.frame = 0
        self.water = None
        self.need_water = False
        self.results = []
        self.error = None
        self.done = False

    def y(self):
        return float(ev(M + '->mPosition.y'))

    def flags(self):
        return int(ev(M + '->mFlag')) & 0xFFFFFFFF

    def put(self, x, y, z):
        for axis, v in (('x', x), ('y', y), ('z', z)):
            setv(M + '->mPosition.' + axis, '%.3f' % v)
        for v in ('mVel.x', 'mVel.y', 'mVel.z', 'mForwardVel'):
            setv(M + '->' + v, '0.0f')

    def stop(self):
        self.waited += 1
        if self.waited > 60000:
            self.done = True
            return True
        if self.settled < 120:
            playing = (int(ev('gpMarDirector->mState')) == 4
                       and not int(ev('gpApplication.mGamePads[0]->mFlags')) & 1
                       and normal.hits > self.normal_hits)
            self.normal_hits = normal.hits
            self.settled = self.settled + 1 if playing else 0
            self.rate = int(ev('port_frame_rate')) // 30
            return False
        if self.check == len(CHECKS):
            self.done = True
            return True
        name, setting, value = CHECKS[self.check]
        r = self.rate
        self.frame += 1
        if name.startswith('jump'):
            airborne = int(ev(M + '->mStatus')) & AIRBORNE
            if self.phase == 'land':
                key(MOUSE_LEFT, False)
                setv(setting, value)
                if airborne:
                    self.frame = 0
                if self.frame >= 30 * r:
                    self.y0 = self.top = self.y()
                    key(MOUSE_LEFT, True)
                    self.phase = 'jump'
                    self.frame = 0
                return False
            self.top = max(self.top, self.y())
            if self.frame >= 10 * r and not airborne:
                key(MOUSE_LEFT, False)
                setv(setting, 1)
                self.results.append((name, self.top - self.y0))
                self.check += 1
                self.phase = 'land'
                self.frame = 0
            elif self.frame > 600 * r:
                self.error = '%s: Mario did not land in 20 seconds' % name
                self.done = True
                return True
            return False
        # under water
        if self.water is None:
            self.need_water = True  # looked for outside the breakpoint
            return True
        x, z, top = self.water
        setv(M + '->mHealth', M + '->mDeParams.mHPMax.value')
        if self.flags() & GAME_OVER:
            self.error = '%s: Mario died (status %#x)' % (name, int(ev(M + '->mStatus')) & 0xFFFFFFFF)
            self.done = True
            return True
        if self.phase == 'land':
            # dropped in from just above the surface
            setv(setting, value)
            setv(M + '->mAir', M + '->mMaxAir')
            self.put(x, top + 100.0, z)
            self.phase = 'drop'
            self.frame = 0
        elif self.phase == 'drop':
            if self.flags() & IN_WATER:
                self.phase = 'hold'
                self.frame = 0
            elif self.frame > 150 * r:
                self.error = '%s: Mario did not reach the water (flags %#x)' % (name, self.flags())
                self.done = True
                return True
        else:
            self.put(x, top - DEPTH, z)
            if self.frame == 300 * r:
                air, full = float(ev(M + '->mAir')), float(ev(M + '->mMaxAir'))
                self.results.append((name, (air, full, self.flags() & IN_WATER)))
                setv(setting, 0)
                self.check += 1
                self.phase = 'land'
                self.frame = 0
        return False


gdb.execute('set confirm off')
normal = NormalCamera('CPolarSubCamera::ctrlNormalOrTowerCamera_')
frames = Frames()
gdb.execute('run')
while not frames.done:
    if not frames.need_water:
        fail('stopped outside the checks')
    frames.need_water = False
    started = time.time()
    try:
        found = find_water(float(ev(M + '->mPosition.x')), float(ev(M + '->mPosition.z')))
    except gdb.error as e:
        fail('looking for water: %s' % e)
    if found is None:
        fail('no water with a floor %d units deep within 4000 units of the start' % (DEPTH + 300))
    frames.water = found
    print('cheats: water at x %.0f z %.0f, surface %.1f (looked for %.0f s)' % (found + (time.time() - started,)),
          flush=True)
    gdb.execute('continue')

res = dict(frames.results)
for name in ('jump, gravity 1', 'jump, gravity 1/2', 'jump, gravity 1/4'):
    if name in res:
        print('cheats: %-22s rose %7.1f' % (name, res[name]), flush=True)
for name in ('under water, air off', 'under water, air on'):
    if name in res:
        air, full, water = res[name]
        print('cheats: %-22s air %.4f of %.4f after 10 seconds, in water %s' % (name, air, full, bool(water)),
              flush=True)
if frames.error:
    fail(frames.error)
if frames.check != len(CHECKS):
    fail('stopped after %d of %d checks (settled %d frames)' % (frames.check, len(CHECKS), frames.settled))

rise = res['jump, gravity 1']
checks = [
    ('a jump rises', rise > 100),
    ('half gravity: about twice as high', 1.8 < res['jump, gravity 1/2'] / rise < 2.2),
    ('quarter gravity: about four times as high', 3.5 < res['jump, gravity 1/4'] / rise < 4.5),
    ('Mario was in the water', res['under water, air off'][2] and res['under water, air on'][2]),
    ('without Infinite Air the meter goes down', res['under water, air off'][0] < res['under water, air off'][1] - 0.05),
    ('with Infinite Air the meter stays full', res['under water, air on'][0] == res['under water, air on'][1]),
]
ok = True
for what, good in checks:
    print('cheats: %-45s %s' % (what, 'ok' if good else 'WRONG'), flush=True)
    ok &= bool(good)
print('cheats: %s' % ('passed' if ok else 'FAILED'), flush=True)
gdb.execute('kill')
gdb.execute('quit %d' % (0 if ok else 1))
