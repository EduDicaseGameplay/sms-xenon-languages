# Run under gdb on regular Sunshine with the nozzle check's environment but
# SMS_WARP=58,1,1 (Mecha-Bowser's coaster ride), headless. See
# docs/DEVELOPMENT.md for the invocation.
# Mouse look on the coaster (decomp-patches/zzz-camera-08-coaster-mouse.patch)
# and dropped mouse motion (platform/pad, g_mouseTaken): moves the mouse
# through the pad layer's motion (g_mouseDX/DY) while the car rides and checks
# that its up and down tilt the aim view (and FLUDD's aim, which follows the
# camera's pitch) as they tilt the Y close-up camera, and move the view from
# behind the car (Y switches) the way the C-stick does, as the normal camera
# takes it. Every pixel moved while
# the coaster camera reads it must reach it. Motion made while no camera takes
# it (the stage's opening, before the ride's camera runs) must be dropped, not
# handed to the first camera that asks. No game code is skipped.
import gdb

PAD = "'(anonymous namespace)::%s'"
KEY_E = 8          # Y's default key: switches the coaster's two views
KEY_W = 26         # STICK_UP's default key
KEY_I = 12         # CSTICK_UP's
KEY_J = 13         # CSTICK_LEFT's
HOLD = 20          # frames each check moves the mouse or holds a key
REST = 20          # frames between checks
PIXELS = 12        # mouse motion per frame


def ev(e):
    return gdb.parse_and_eval(e)


def pad(name):
    return ev(PAD % name)


def setv(name, value, index=None):
    target = PAD % name + ('' if index is None else '[%d]' % index)
    gdb.execute('set var %s = %d' % (target, value), to_string=True)


def fail(message):
    print('coaster_mouse: error: ' + message, flush=True)
    gdb.execute('kill')
    gdb.execute('quit 1')


def aim_view():
    return int(ev('gpCamera->unk2B8->mLButtonMode')) & 1


def values():
    w = 'gpMarioOriginal->mWaterGun'
    return {
        'pitch': int(ev('gpCamera->mCurrentTarget.mPitch')),  # the aim view's tilt
        'fludd': int(ev('%s->mNozzleList[%s->mCurrentNozzle]->unk36E' % (w, w))),
        'offX': int(ev('gpCamera->unk2B8->unk8')),   # the behind view's tilt goal
        'offY': int(ev('gpCamera->unk2B8->unkA')),   # and its turn goal
    }


# Each check: name, the view it needs (1 aim, 0 behind), mouse dx, dy per
# frame, a key held instead, and what must move: a value, or for 'same' the
# check whose motion it must repeat (+1) or reverse (-1). Each mouse check
# undoes the stick's before it, so neither stops at a limit: the mouse's up is
# the close-up camera's (the control stick's down, FLUDD aimed up), its down
# the C-stick's down and its left the C-stick's right, as the normal camera
# takes them.
CHECKS = [
    ('aim, control stick up', 1, 0, 0, KEY_W, ('pitch', 0)),
    ('aim, mouse up', 1, 0, -PIXELS, None, ('same', 'aim, control stick up', -1)),
    ('aim, mouse down', 1, 0, +PIXELS, None, ('same', 'aim, mouse up', -1)),
    ('aim, mouse right (the car turns it)', 1, +PIXELS, 0, None, ('none', 0)),
    ('switch to behind view', 0, 0, 0, KEY_E, ('view', 0)),
    ('behind, C up', 0, 0, 0, KEY_I, ('offX', 0)),
    ('behind, mouse down', 0, 0, +PIXELS, None, ('same', 'behind, C up', -1)),
    ('behind, C left', 0, 0, 0, KEY_J, ('offY', 0)),
    ('behind, mouse left', 0, -PIXELS, 0, None, ('same', 'behind, C left', -1)),
    ('switch to aim view', 1, 0, 0, KEY_E, ('view', 1)),
]


class Coaster(gdb.Breakpoint):
    hits = 0

    def stop(self):
        self.hits += 1
        return False


class Take(gdb.Breakpoint):
    # every call of port_camera_take_mouse: the motion it finds waiting
    def __init__(self):
        super().__init__('port_camera_take_mouse')
        self.calls = 0
        self.got_x = 0
        self.got_y = 0
        self.first = None

    def stop(self):
        if int(ev('port_coop_in_p2')):
            return False
        self.calls += 1
        dx, dy = int(pad('g_mouseDX')), int(pad('g_mouseDY'))
        self.got_x += dx
        self.got_y += dy
        if self.first is None:
            self.first = (dx, dy)
        return False


class Frames(gdb.Breakpoint):
    def __init__(self):
        super().__init__('TMarDirector::direct')
        self.waited = 0
        self.settled = 0
        self.coaster_hits = 0
        self.take_calls = 0
        # motion made before any camera takes it
        self.early_frames = 0
        self.early_sent = 0
        self.check = 0
        self.frame = 0
        self.start = None
        self.sent = [0, 0]
        self.taken_at_start = None
        self.results = {}
        self.order = []
        self.error = None

    def release(self):
        for k in (KEY_E, KEY_W, KEY_I, KEY_J):
            setv('g_key', 0, k)

    def stop(self):
        self.waited += 1
        if self.waited > 40000:
            self.error = 'timed out (coaster camera ran %d times)' % coaster.hits
            return True
        playing = int(ev('gpMarDirector->mState')) == 4
        # Until a camera takes the mouse in the stage: move it every frame
        # (the stage's opening). Only the last frames' worth may reach the
        # camera when it starts.
        if take.calls == 0:
            if playing:
                setv('g_mouseDX', int(pad('g_mouseDX')) + 40)
                self.early_frames += 1
                self.early_sent += 40
            return False
        if self.settled < 120:
            ok = playing and coaster.hits > self.coaster_hits
            self.coaster_hits = coaster.hits
            self.settled = self.settled + 1 if ok else 0
            return False
        if coaster.hits == self.coaster_hits:
            self.error = 'the coaster camera stopped running during "%s"' % CHECKS[self.check][0]
            return True
        self.coaster_hits = coaster.hits
        if self.check == len(CHECKS):
            return True
        name, view, dx, dy, key, moves = CHECKS[self.check]
        if self.frame == 0:
            if key != KEY_E and aim_view() != view:
                self.error = '"%s" needs the %s view' % (name, 'aim' if view else 'behind')
                return True
            self.start = values()
            self.sent = [0, 0]
            self.taken_at_start = (take.got_x, take.got_y)
            if key is not None:
                setv('g_key', 1, key)
        if 0 <= self.frame < HOLD and key is None:
            setv('g_mouseDX', int(pad('g_mouseDX')) + dx)
            setv('g_mouseDY', int(pad('g_mouseDY')) + dy)
            self.sent[0] += dx
            self.sent[1] += dy
        if key == KEY_E and self.frame == 2:
            self.release()
        if self.frame == HOLD:
            self.release()
        if self.frame == HOLD + REST // 2:
            end = values()
            got = (take.got_x - self.taken_at_start[0], take.got_y - self.taken_at_start[1])
            self.results[name] = (moves, {k: end[k] - self.start[k] for k in end},
                                  tuple(self.sent), got, aim_view())
            self.order.append(name)
        self.frame += 1
        if self.frame == HOLD + REST:
            self.frame = 0
            self.check += 1
        return False


gdb.execute('set confirm off')
coaster = Coaster('CPolarSubCamera::ctrlJetCoasterCamera_')
take = Take()
frames = Frames()
gdb.execute('run')
if frames.error:
    fail(frames.error)
if frames.check != len(CHECKS):
    fail('stopped after %d of %d checks' % (frames.check, len(CHECKS)))

ok = True
first = take.first or (0, 0)
# the camera's first call may find at most kMouseKeepReads frames of it
good = frames.early_frames > 30 and abs(first[0]) <= 40 * 3
print('coaster_mouse: %-36s moved %d px over %d frames, first take found %+d px  %s'
      % ('motion before any camera', frames.early_sent, frames.early_frames, first[0],
         'ok' if good else 'WRONG'), flush=True)
ok &= good
for name in frames.order:
    moves, d, sent, got, view = frames.results[name]
    kind = moves[0]
    if kind == 'none':
        good = abs(d['pitch']) < 1 and d['offX'] == 0 and d['offY'] == 0
    elif kind == 'view':
        good = view == moves[1]
    elif kind in ('pitch', 'offX', 'offY'):
        good = abs(d[kind]) > 50
    else:
        ref = frames.results[moves[1]][1]
        k = max(('pitch', 'offX', 'offY'), key=lambda n: abs(ref[n]))
        good = abs(d[k]) > 50 and (d[k] > 0) == ((ref[k] > 0) == (moves[2] > 0))
    # FLUDD aims where the aim view looks (TNozzleBase::calcGunAngle)
    if view and kind != 'view' and abs(d['fludd'] - d['pitch']) > 1:
        good = False
    # all the motion sent while the coaster camera runs reaches it
    if sent != (0, 0) and got != sent:
        good = False
    print('coaster_mouse: %-36s pitch %+5d FLUDD %+5d  tilt goal %+5d turn goal %+5d  sent %s got %s  %s'
          % (name, d['pitch'], d['fludd'], d['offX'], d['offY'], sent, got, 'ok' if good else 'WRONG'),
          flush=True)
    ok &= good
print('coaster_mouse: %s' % ('passed' if ok else 'FAILED'), flush=True)
gdb.execute('kill')
gdb.execute('quit %d' % (0 if ok else 1))
