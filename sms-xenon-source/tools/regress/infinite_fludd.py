# Run under gdb with a fresh memory card and SMS_WARP=2,7,1 (Bianco Hills),
# headless. See docs/DEVELOPMENT.md for the invocation.
# The F9 menu's Infinite FLUDD (decomp-patches/port-07-cheat-infinite-fludd.patch)
# with the Rocket and the Hover nozzle: holds R (Space) through the pad layer's
# key state from a full tank, with the cheat off and on. The Rocket must go as
# high with the cheat as without it (its timer ends the launch), and the Hover
# must keep hovering while R is held with the cheat, past where it stops
# without it. No game code is skipped.
import gdb

PAD = "'(anonymous namespace)::%s'"
M = 'gpMarioOriginal'
SPACE = 44      # R (Space) in the default bindings
ROCKET, HOVER = 1, 4
STATUS_ROCKET = 0x88B
AIRBORNE = 0x800

# name, nozzle, Infinite FLUDD; each starts where Mario stood at the first.
# The run's first Rocket goes about 50 units lower than the ones after it,
# cheat or not, so it is only a warm-up.
CHECKS = [
    ('Rocket, warm-up', ROCKET, 0),
    ('Rocket, cheat off', ROCKET, 0),
    ('Rocket, cheat on', ROCKET, 1),
    ('Hover, cheat off', HOVER, 0),
    ('Hover, cheat on', HOVER, 1),
]


def ev(e):
    return gdb.parse_and_eval(e)


def setv(name, value):
    gdb.execute('set var %s = %s' % (name, value), to_string=True)


def fail(message):
    print('infinite_fludd: error: ' + message, flush=True)
    gdb.execute('kill')
    gdb.execute('quit 1')


class NormalCamera(gdb.Breakpoint):
    # counts the frames the normal camera runs (Bianco's opening cameras and
    # the file select come first)
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
        self.phase = 'switch'
        self.frame = 0
        self.call = None
        self.home = None
        self.results = []
        self.error = None
        self.done = False

    def begin(self):
        self.y0 = float(ev(M + '->mPosition.y'))
        self.top = self.y0
        self.rocket_frames = 0
        self.min_water = int(ev(M + '->mWaterGun->mCurrentWater'))

    def track(self):
        self.top = max(self.top, float(ev(M + '->mPosition.y')))
        if int(ev(M + '->mStatus')) & 0xFFFFFFFF == STATUS_ROCKET:
            self.rocket_frames += 1
        self.min_water = min(self.min_water, int(ev(M + '->mWaterGun->mCurrentWater')))

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
        name, nozzle, cheat = CHECKS[self.check]
        r = self.rate
        self.frame += 1
        if self.phase == 'switch':
            # the nozzle is changed outside the breakpoint, then a full tank
            setv(PAD % 'g_key' + '[%d]' % SPACE, 0)
            setv('port_cheat_infinite_fludd', cheat)
            self.call = 'call %s->mWaterGun->changeNozzle((TWaterGun::TNozzleType)%d, false)' % (M, nozzle)
            if self.home is None:
                self.home = [float(ev(M + '->mPosition.' + axis)) for axis in 'xyz']
            for axis, v in zip('xyz', self.home):
                setv(M + '->mPosition.' + axis, '%.6f' % v)
            for v in ('mVel.x', 'mVel.y', 'mVel.z', 'mForwardVel'):
                setv(M + '->' + v, '0.0f')
            self.phase = 'land'
            self.frame = 0
            return True
        if self.phase == 'land':
            # standing still on the ground for three seconds
            setv(M + '->mHealth', M + '->mDeParams.mHPMax.value')
            setv(M + '->mWaterGun->mCurrentWater', M + '->mWaterGun->mNozzleList[%d]->mEmitParams.mAmountMax.value' % nozzle)
            if int(ev(M + '->mStatus')) & AIRBORNE:
                self.frame = 0
            if self.frame >= 90 * r:
                self.begin()
                setv(PAD % 'g_key' + '[%d]' % SPACE, 1)
                self.phase = 'hold'
                self.frame = 0
            return False
        self.track()
        if self.phase == 'hold' and self.frame == 150 * r:
            self.hold_rocket = self.rocket_frames
            setv(PAD % 'g_key' + '[%d]' % SPACE, 0)
            self.phase = 'fall'
            self.frame = 0
        elif self.phase == 'fall' and self.frame >= 30 * r and not int(ev(M + '->mStatus')) & AIRBORNE:
            self.results.append((CHECKS[self.check], self.top - self.y0, self.hold_rocket / r, self.min_water, self.y0))
            self.check += 1
            self.phase = 'switch'
        elif self.phase == 'fall' and self.frame > 900 * r:
            self.results.append((CHECKS[self.check], self.top - self.y0, self.hold_rocket / r, self.min_water, self.y0))
            self.error = '%s: Mario did not land in 30 seconds' % name
            self.done = True
            return True
        return False


gdb.execute('set confirm off')
normal = NormalCamera('CPolarSubCamera::ctrlNormalOrTowerCamera_')
frames = Frames()
gdb.execute('run')
while not frames.done:
    if frames.call is None:
        fail('stopped outside the checks')
    try:
        gdb.execute(frames.call, to_string=True)
    except gdb.error as e:
        fail('%s: %s' % (frames.call, e))
    frames.call = None
    gdb.execute('continue')
res = {c[0]: (rise, rocketing, water, y0) for c, rise, rocketing, water, y0 in frames.results}
if frames.home:
    print('infinite_fludd: each check starts at x %.1f y %.1f z %.1f' % tuple(frames.home), flush=True)
for name, (rise, rocketing, water, y0) in res.items():
    print('infinite_fludd: %-20s from y %.1f rose %7.1f, %5.1f frames (30 fps) in the Rocket/Hover status while R '
          'was held, tank at least %d' % (name, y0, rise, rocketing, water), flush=True)
if frames.error:
    fail(frames.error)
if frames.check != len(CHECKS):
    fail('stopped after %d of %d checks (settled %d frames)' % (frames.check, len(CHECKS), frames.settled))
checks = [
    ('the Rocket launches', res['Rocket, cheat off'][0] > 300),
    ('the Rocket goes as high with Infinite FLUDD as without it',
     abs(res['Rocket, cheat on'][0] - res['Rocket, cheat off'][0]) < 0.01),
    ('the Rocket with Infinite FLUDD uses no water',
     res['Rocket, cheat on'][2] == int(ev(M + '->mWaterGun->mNozzleList[1]->mEmitParams.mAmountMax.value'))),
    ('the Hover stops by its timer without the cheat', res['Hover, cheat off'][1] < 140),
    ('the Hover lasts while R is held with Infinite FLUDD', res['Hover, cheat on'][1] >= 140),
]
ok = True
for what, good in checks:
    print('infinite_fludd: %-60s %s' % (what, 'ok' if good else 'WRONG'), flush=True)
    ok &= good
print('infinite_fludd: %s' % ('passed' if ok else 'FAILED'), flush=True)
gdb.execute('kill')
gdb.execute('quit %d' % (0 if ok else 1))
