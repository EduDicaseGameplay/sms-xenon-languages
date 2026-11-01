# Run under gdb with SMS_CSTICK_ORBIT=1, a fresh memory card and SMS_WARP=2,7,1
# (Bianco Hills), headless. See docs/DEVELOPMENT.md for the invocation.
# C-stick orbit (decomp-patches/zzz-camera-06-cstick-orbit.patch): holds the
# C-stick's up or down through the pad layer for a while and checks what moved:
# with the setting on, the tilt (port_camera_mouse_pitch) moves and the camera's
# distance ratio (mCurrentTarget.unk28, the GameCube's nearer or further) does
# not; with CAMERA_ZOOM held (the right stick's press, the default binding),
# and with the setting off, it is the other way round. Invert camera Y flips
# the tilt's direction. No game code is skipped.
import gdb

PAD = "'(anonymous namespace)::%s'"
KEY_I = 12        # CSTICK_UP's default key
KEY_K = 14        # CSTICK_DOWN's
PAD_RSTICK = 8    # CAMERA_ZOOM's default controller button
HOLD = 20         # frames each check holds the stick
REST = 15         # frames between checks


def ev(e):
    return gdb.parse_and_eval(e)


def setv(name, value, index=None):
    target = PAD % name + ('' if index is None else '[%d]' % index)
    gdb.execute('set var %s = %d' % (target, value), to_string=True)


def fail(message):
    print('cstick_orbit: error: ' + message, flush=True)
    gdb.execute('kill')
    gdb.execute('quit 1')


# Each check: its name, the setting, Invert camera Y, the key held, whether
# CAMERA_ZOOM is held; and what must move (tilt or distance).
CHECKS = [
    ('orbit, C up', 1, 0, KEY_I, 0, 'tilt', -1),
    # down first: the camera may start as near as it goes
    ('orbit, C down, CAMERA_ZOOM held', 1, 0, KEY_K, 1, 'distance', 0),
    ('orbit, C up, Invert camera Y', 1, 1, KEY_I, 0, 'tilt', +1),
    ('orbit, C down', 1, 0, KEY_K, 0, 'tilt', +1),
    ('orbit off, C up', 0, 0, KEY_I, 0, 'distance', 0),
]


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
        self.orbit_seen = set()
        self.results = []
        self.waited = 0
        self.normal_hits = 0
        self.error = None

    def values(self):
        return (float(ev('port_camera_mouse_pitch')), float(ev('gpCamera->mCurrentTarget.unk28')))

    def release(self):
        setv('g_key', 0, KEY_I)
        setv('g_key', 0, KEY_K)
        setv('g_cbtn', 0, PAD_RSTICK)

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
        if normal.hits == self.normal_hits:
            self.error = 'the normal camera stopped reading the sticks during "%s"' % CHECKS[self.check][0]
            return True
        self.normal_hits = normal.hits
        if self.check == len(CHECKS):
            return True
        name, orbit, invert, key, zoom, moves, sign = CHECKS[self.check]
        if self.frame == 0:
            setv('g_cstickOrbit', orbit)
            setv('g_invert_cy', invert)
            setv('g_key', 1, key)
            setv('g_cbtn', zoom, PAD_RSTICK)
            # level the tilt, so the camera's range for it (calcPosAndAt_) cannot clamp it
            gdb.execute('set var port_camera_mouse_pitch = 0', to_string=True)
            self.start = self.values()
            self.orbit_seen = set()
        elif self.frame <= HOLD:
            self.orbit_seen.add(int(ev('port_cstick_orbit')))
        if self.frame == HOLD:
            end = self.values()
            self.results.append((name, moves, sign, end[0] - self.start[0], end[1] - self.start[1],
                                 sorted(self.orbit_seen)))
            self.release()
        self.frame += 1
        if self.frame == HOLD + REST:
            self.frame = 0
            self.check += 1
        return False


gdb.execute('set confirm off')
normal = NormalCamera('CPolarSubCamera::ctrlNormalOrTowerCamera_')
frames = Frames()
gdb.execute('run')
if frames.error:
    fail(frames.error)
if frames.check != len(CHECKS):
    fail('stopped after %d of %d checks (settled %d frames)' % (frames.check, len(CHECKS), frames.settled))
ok = True
for name, moves, sign, tilt, dist, orbit in frames.results:
    good = (abs(dist) < 1e-4 and abs(tilt) > 50 and (tilt > 0) == (sign > 0)) if moves == 'tilt' \
        else (abs(dist) > 0.01 and abs(tilt) < 1.0)
    print('cstick_orbit: %-32s tilt %+8.1f  distance ratio %+.4f  port_cstick_orbit %s  %s'
          % (name, tilt, dist, orbit, 'ok' if good else 'WRONG'), flush=True)
    ok &= good
print('cstick_orbit: %s' % ('passed' if ok else 'FAILED'), flush=True)
gdb.execute('kill')
gdb.execute('quit %d' % (0 if ok else 1))
