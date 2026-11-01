# Run under gdb with a fresh memory card and SMS_WARP=2,7,1 (Bianco Hills),
# headless. See docs/DEVELOPMENT.md for the invocation.
# Aim with the C-stick in the spray-in-place stance
# (decomp-patches/zzz-camera-07-stance-cstick-aim.patch): holds keys through
# the pad layer's key state and checks what moved. With the setting on, R all
# the way down (Space) and the C-stick turn Mario, the camera keeps up with him, and
# the C-stick's up and down tilt FLUDD the way they tilt the Y close-up
# camera (also checked); with L (Left Shift) the control stick walks him
# sideways while the C-stick turns him. With the setting off the C-stick turns
# the camera and not Mario, as on the GameCube. No game code is skipped.
# Invert FLUDD aim Y (SMS_FLUDD_AIM_INVERT_Y, decomp-patches/zzz-camera-03):
# the control stick's up tilts FLUDD down in the stance (as on the GameCube),
# up with it on, whatever Invert camera Y says, and the Y close-up camera's
# control stick stays as it is. At the end, with the game stopped, it checks
# what the environment set it to (unset: as Invert camera Y) and that the
# settings menu's Invert Camera Y moves it only until its own switch is used.
# STANCE_AIM_ENV_ONLY=1 checks only that, as soon as Mario can move.
# Mouse aim in the stance (decomp-patches/zzz-camera-08-stance-mouse-aim.patch):
# with Mouse Aim in Mario Cam on (the default) the mouse's motion, given to the
# pad layer a frame at a time as mouse look would, turns Mario and tilts FLUDD
# in the stance as the C-stick does with its setting, and the camera follows
# him; off, or standing, it turns the camera. Invert camera X leaves the mouse
# alone (it still flips the C-stick); Invert camera Y flips its tilt.
import os
import gdb

PAD = "'(anonymous namespace)::%s'"
# SDL scancodes of the default bindings
SPACE, LSHIFT, KEY_D, KEY_E = 44, 225, 7, 8          # R, L, STICK_RIGHT, Y
KEY_W = 26                                           # STICK_UP
KEY_I, KEY_K, KEY_L = 12, 14, 15                     # CSTICK_UP, _DOWN, _RIGHT
ENTER = 20      # frames the stance (or close-up camera) gets before the C-stick
HOLD = 30       # frames the C-stick is held
REST = 45       # frames between checks, everything released
SQUAT = 0xC008220


def ev(e):
    return gdb.parse_and_eval(e)


def setv(name, value, index=None):
    target = PAD % name + ('' if index is None else '[%d]' % index)
    gdb.execute('set var %s = %d' % (target, value), to_string=True)


def fail(message):
    print('stance_aim: error: ' + message, flush=True)
    gdb.execute('kill')
    gdb.execute('quit 1')


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


# name, aim setting, Invert camera Y, Invert FLUDD aim Y, keys held from the
# start, the key held for the check (a C-stick's, or the control stick's up),
# toggle the close-up camera with Y, and optionally the mouse: its motion each
# frame of the hold (pixels; up is negative), Mouse Aim in Mario Cam (default
# on) and Invert camera X (default off)
MOUSE_PX = 10
CHECKS = [
    ('on, R, C right', 1, 0, 0, [SPACE], KEY_L, False),
    ('on, R, C up', 1, 0, 0, [SPACE], KEY_I, False),
    ('on, R, C up, Invert Y', 1, 1, 1, [SPACE], KEY_I, False),
    ('on, close-up camera, C up', 1, 0, 0, [], KEY_I, True),
    ('on, L + stick right, C right', 1, 0, 0, [LSHIFT, KEY_D], KEY_L, False),
    ('on, standing, C right', 1, 0, 0, [], KEY_L, False),
    ('off, R, C right', 0, 0, 0, [SPACE], KEY_L, False),
    ('off, R, C down', 0, 0, 0, [SPACE], KEY_K, False),
    ('off, R, stick up', 0, 0, 0, [SPACE], KEY_W, False),
    ('off, R, stick up, Invert FLUDD Y', 0, 0, 1, [SPACE], KEY_W, False),
    ('off, R, stick up, Invert camera Y', 0, 1, 0, [SPACE], KEY_W, False),
    ('on, R, stick up, Invert FLUDD Y', 1, 0, 1, [SPACE], KEY_W, False),
    ('off, close-up camera, stick up', 0, 0, 0, [], KEY_W, True),
    ('off, close-up camera, stick up, Invert FLUDD Y', 0, 0, 1, [], KEY_W, True),
    ('off, standing, C right, Invert X', 0, 0, 0, [], KEY_L, False, {'invx': 1}),
    ('mouse, R, right', 0, 0, 0, [SPACE], None, False, {'dx': MOUSE_PX}),
    ('mouse, R, up', 0, 0, 0, [SPACE], None, False, {'dy': -MOUSE_PX}),
    ('mouse, R, up, Invert Y', 0, 1, 0, [SPACE], None, False, {'dy': -MOUSE_PX}),
    ('mouse, L + stick right, right', 0, 0, 0, [LSHIFT, KEY_D], None, False, {'dx': MOUSE_PX}),
    ('mouse off, R, right', 0, 0, 0, [SPACE], None, False, {'dx': MOUSE_PX, 'close': 0}),
    ('mouse, standing, right', 0, 0, 0, [], None, False, {'dx': MOUSE_PX}),
    ('mouse, standing, right, Invert X', 0, 0, 0, [], None, False, {'dx': MOUSE_PX, 'invx': 1}),
]
ENV_ONLY = os.environ.get('STANCE_AIM_ENV_ONLY') == '1'


def water_vy():
    # the mean upward share of the live water particles' speed: which way the
    # spray goes, whatever the gun angle's sign means
    w = ev('gpModelWaterManager')
    n = int(w['mParticleCount'])
    total = 0.0
    for i in range(n):
        v = w['mParticleVelocitySOA'][i]
        x, y, z = float(v['x']), float(v['y']), float(v['z'])
        length = (x * x + y * y + z * z) ** 0.5
        total += y / length if length else 0.0
    return total / n if n else 0.0


def values():
    m = '((TMario*)gpMarioAddress)'
    return {
        'yaw': int(ev(m + '->mFaceAngle.y')),
        'cam': int(ev('gpCamera->mCurrentTarget.mYaw')),
        'ratio': float(ev('gpCamera->mCurrentTarget.unk28')),
        'gun': int(ev(m + '->mWaterGun->mNozzleList[' + m + '->mWaterGun->mCurrentNozzle]->unk36E')),
        'x': float(ev(m + '->mPosition.x')),
        'z': float(ev(m + '->mPosition.z')),
        'status': int(ev(m + '->mStatus')) & 0xFFFFFFFF,
        'water': int(ev(m + '->mWaterGun->mCurrentWater')),
        'mode': int(ev('gpCamera->mMode')),
    }


class NormalCamera(gdb.Breakpoint):
    # counts the frames the normal camera reads the sticks (Bianco's opening
    # cameras and the file select come first)
    hits = 0

    def stop(self):
        self.hits += 1
        return False


class Frames(gdb.Breakpoint):
    def __init__(self):
        super().__init__('TMarDirector::direct')
        self.settled = 0
        self.check = 0
        self.frame = 0
        self.start = None
        self.statuses = set()
        self.results = []
        self.waited = 0
        self.normal_hits = 0
        self.error = None
        self.initial = None

    def keys(self, codes, down):
        for k in codes:
            setv('g_key', 1 if down else 0, k)

    def stop(self):
        self.waited += 1
        if self.waited > 30000:
            return True
        if self.settled < 120:
            playing = (int(ev('gpMarDirector->mState')) == 4
                       and not int(ev('gpApplication.mGamePads[0]->mFlags')) & 1
                       and normal.hits > self.normal_hits)
            self.normal_hits = normal.hits
            self.settled = self.settled + 1 if playing else 0
            return False
        if self.initial is None:
            self.initial = {n: int(ev(PAD % n)) for n in ('g_invert_cy', 'g_invertFluddY',
                                                         'g_fluddYFollowsCamera')}
            self.initial['port_fludd_aim_invert_y'] = int(ev('port_fludd_aim_invert_y'))
        if ENV_ONLY or self.check == len(CHECKS):
            return True
        name, aim, invert, fludd, held, cstick, closeup = CHECKS[self.check][:7]
        mouse = CHECKS[self.check][7] if len(CHECKS[self.check]) > 7 else {}
        f = self.frame
        if f == 0:
            setv('g_closeCameraCStick', aim)
            setv('g_invert_cy', invert)
            setv('g_invertFluddY', fludd)
            setv('g_closeCameraMouse', mouse.get('close', 1))
            setv('g_invert_cx', mouse.get('invx', 0))
            # a full tank, so the stance holds while it sprays
            gdb.execute('set var ((TMario*)gpMarioAddress)->mWaterGun->mCurrentWater = '
                        '((TMario*)gpMarioAddress)->mWaterGun->mNozzleList[0]->mEmitParams.mAmountMax.value',
                        to_string=True)
            self.keys(held, True)
            if closeup:
                self.keys([KEY_E], True)
            self.statuses = set()
        elif f == 3 and closeup:
            self.keys([KEY_E], False)
        elif f == ENTER - 1 and mouse:
            # the camera behind him, where it was in the C-stick's checks, so
            # that following him keeps it there
            gdb.execute('set var gpCamera->mCurrentTarget.mYaw = '
                        '(short)(((TMario*)gpMarioAddress)->mFaceAngle.y - 0x8000)', to_string=True)
        elif f == ENTER:
            self.start = values()
            if cstick is not None:
                self.keys([cstick], True)
        elif ENTER < f <= ENTER + HOLD:
            self.statuses.add(values()['status'])
        if ENTER <= f < ENTER + HOLD and mouse:
            # this frame's motion, which the camera takes (port_camera_take_mouse)
            setv('g_mouseDX', mouse.get('dx', 0))
            setv('g_mouseDY', mouse.get('dy', 0))
        if f == ENTER + HOLD:
            end = values()
            end['vy'] = water_vy()
            self.results.append((CHECKS[self.check], self.start, end, sorted(self.statuses)))
            self.keys(held + ([cstick] if cstick is not None else []), False)
            if closeup:
                self.keys([KEY_E], True)
        if f == ENTER + HOLD + 3 and closeup:
            self.keys([KEY_E], False)
        self.frame += 1
        if self.frame == ENTER + HOLD + REST:
            self.frame = 0
            self.check += 1
        return False


gdb.execute('set confirm off')
normal = NormalCamera('CPolarSubCamera::ctrlNormalOrTowerCamera_')
frames = Frames()
gdb.execute('run')
if frames.error:
    fail(frames.error)
if frames.initial is None:
    fail('Mario never could move (settled %d frames)' % frames.settled)


def env_on(name):
    e = os.environ.get(name)
    return 1 if e and e != '0' else 0


def menu(fn, value):
    gdb.execute("call (int)'(anonymous namespace)::%s'(%d)" % (fn, value), to_string=True)
    return int(ev(PAD % 'g_invertFluddY'))


# what the environment (also the game's) set it to, then the menu's switches
fludd_env = os.environ.get('SMS_FLUDD_AIM_INVERT_Y')
follows = 0 if fludd_env else 1
want = env_on('SMS_CAMERA_INVERT_Y') if follows else env_on('SMS_FLUDD_AIM_INVERT_Y')
init = frames.initial
good = (init['g_fluddYFollowsCamera'] == follows and init['g_invertFluddY'] == want
        and init['port_fludd_aim_invert_y'] == want
        and init['g_invert_cy'] == env_on('SMS_CAMERA_INVERT_Y'))
print('stance_aim: %-30s camera Y %d, FLUDD Y %d (in use %d), follows the camera %d  %s'
      % ('environment', init['g_invert_cy'], init['g_invertFluddY'], init['port_fludd_aim_invert_y'],
         init['g_fluddYFollowsCamera'], 'ok' if good else 'WRONG'), flush=True)
ok = good
setv('g_fluddYFollowsCamera', 1)
setv('g_invertFluddY', 0)
seen = [menu('invert_y_set', 1), menu('invert_y_set', 0), menu('invert_fludd_y_set', 1),
        menu('invert_y_set', 0), menu('invert_fludd_y_set', 0), menu('invert_y_set', 1)]
good = seen == [1, 0, 1, 1, 0, 0] and int(ev(PAD % 'g_fluddYFollowsCamera')) == 0
print('stance_aim: %-30s FLUDD Y after camera on, off, FLUDD on, camera off, FLUDD off, camera on: %s  %s'
      % ('menu', seen, 'ok' if good else 'WRONG'), flush=True)
ok &= good
if ENV_ONLY:
    print('stance_aim: %s' % ('passed' if ok else 'FAILED'), flush=True)
    gdb.execute('kill')
    gdb.execute('quit %d' % (0 if ok else 1))
if frames.check != len(CHECKS):
    fail('stopped after %d of %d checks (settled %d frames)' % (frames.check, len(CHECKS), frames.settled))

gun = {}
vy = {}
for check, a, b, statuses in frames.results:
    name, aim, invert, fludd, held, cstick, closeup = check[:7]
    d = {
        'yaw': s16(b['yaw'] - a['yaw']),
        'cam': s16(b['cam'] - a['cam']),
        'ratio': b['ratio'] - a['ratio'],
        'gun': b['gun'] - a['gun'],
        'move': ((b['x'] - a['x']) ** 2 + (b['z'] - a['z']) ** 2) ** 0.5,
    }
    gun[name] = d['gun']
    vy[name] = b['vy']
    behind = s16(b['cam'] - (b['yaw'] - 0x8000))
    stance = statuses == [SQUAT]
    if name == 'on, R, C right':
        # the camera keeps up with him
        good = stance and d['yaw'] > 1000 and abs(behind) < d['yaw'] / 4
    elif name == 'on, R, C up':
        good = stance and d['gun'] < -100 and d['yaw'] == 0 and abs(d['ratio']) < 1e-4
    elif name == 'on, R, C up, Invert Y':
        good = stance and d['gun'] > 100 and d['yaw'] == 0
    elif name == 'on, close-up camera, C up':
        # the Y close-up camera's mode (CAMERA_MODE_L_NORMAL) through the hold,
        # and the C-stick's up tilts it, and FLUDD, the same way as in the stance
        good = (a['mode'] == b['mode'] == int(ev('(int)CAMERA_MODE_L_NORMAL'))
                and d['gun'] < -100)
    elif name == 'on, L + stick right, C right':
        good = stance and d['yaw'] > 1000 and d['move'] > 20 and abs(behind) < d['yaw'] / 4
    elif name in ('off, R, stick up', 'off, R, stick up, Invert camera Y'):
        # as on the GameCube, the control stick's up tilts FLUDD down
        # (a larger gun angle), Invert camera Y or not
        good = stance and d['gun'] > 100 and d['yaw'] == 0
    elif name in ('off, R, stick up, Invert FLUDD Y', 'on, R, stick up, Invert FLUDD Y'):
        # and the water goes higher than with the stick's up as on the GameCube
        good = stance and d['gun'] < -100 and d['yaw'] == 0
        if name.startswith('off'):
            good = good and b['vy'] > vy['off, R, stick up'] + 0.1
    elif name.startswith('off, close-up camera, stick up'):
        # the close-up camera's control stick is left alone
        good = a['mode'] == b['mode'] == int(ev('(int)CAMERA_MODE_L_NORMAL'))
        if name.endswith('Invert FLUDD Y'):
            good = good and abs(d['gun'] - gun['off, close-up camera, stick up']) <= 64
        else:
            good = good and abs(d['gun']) > 100
    elif name == 'off, standing, C right, Invert X':
        # Invert camera X still flips the C-stick
        good = not stance and d['yaw'] == 0 and d['cam'] < -1000
    elif name == 'mouse, R, right':
        # the mouse's right turns him right (the C-stick's left), and the
        # camera keeps up with him
        good = stance and d['yaw'] < -1000 and abs(behind) < -d['yaw'] / 4
    elif name == 'mouse, R, up':
        # the mouse's up tilts FLUDD up, as the C-stick's does
        good = stance and d['gun'] < -100 and d['yaw'] == 0 and abs(d['ratio']) < 1e-4
    elif name == 'mouse, R, up, Invert Y':
        good = stance and d['gun'] > 100 and d['yaw'] == 0
    elif name == 'mouse, L + stick right, right':
        good = stance and d['yaw'] < -1000 and d['move'] > 20 and abs(behind) < -d['yaw'] / 4
    elif name == 'mouse off, R, right':
        good = stance and d['yaw'] == 0 and d['cam'] < -1000
    elif name in ('mouse, standing, right', 'mouse, standing, right, Invert X'):
        # the view turns right whichever way Invert camera X sets the C-stick
        good = not stance and d['yaw'] == 0 and d['cam'] < -1000
    elif name == 'on, standing, C right':
        good = not stance and d['yaw'] == 0 and d['cam'] > 1000
    elif name == 'off, R, C right':
        good = stance and d['yaw'] == 0 and d['cam'] > 1000
    else:  # off, R, C down: the camera's distance moves, FLUDD stays put
        good = stance and abs(d['ratio']) > 0.01 and abs(d['gun']) < 50
    print('stance_aim: %-30s yaw %+6d camera %+6d (behind %+6d) ratio %+.3f FLUDD %+5d (water up %+.2f) '
          'moved %6.1f stance %s  %s' % (name, d['yaw'], d['cam'], behind, d['ratio'], d['gun'], b['vy'],
                                         d['move'], stance, 'ok' if good else 'WRONG'), flush=True)
    if not good:
        print('stance_aim:   statuses %s' % ', '.join('0x%X' % v for v in statuses), flush=True)
    ok &= good
print('stance_aim: %s' % ('passed' if ok else 'FAILED'), flush=True)
gdb.execute('kill')
gdb.execute('quit %d' % (0 if ok else 1))
