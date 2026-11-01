# BetterSunshineMoveset's Long Jump/Crouch Button setting, under gdb
# (docs/DEVELOPMENT.md).
#
# Set to Z (the default) the game is as retail: Z crouches (the moveset's
# crouch, from the pad meaning 0x1000) and L ground pounds in the air (0x2000)
# and centres the camera (0x8000). Set to L, the setting's valueChanged
# rewrites retail instructions in TMarioGamePad::updateMeaning and TMario's
# ground pound tests (PowerPC::writeU32): L then crouches and ground pounds
# (0x1000) and Z has the L meanings (0x2000, 0x8000) and so centres the
# camera. Z does not ground pound: the out-of-line TMario::checkBackTrig then
# takes any of 0x1000 to 0x8000, but only once checkController has seen the
# crouch meaning (or B) start. The port had recorded the rewrites but no hook
# read them, so the setting did nothing.
#
# Run on an Eclipse build with the Eclipse disc, a copy of the card
# regress.py's ecl-firstboot run saves, and SMS_AUTOPRESS through the title,
# the file select and the Tutorial's dialogue (the A presses of regress.py's
# ECL_TUTORIAL_AP up to field 2380). Once Mario stands still it holds keys
# through the pad layer's key state: L, then Z, in a jump and standing, with
# the setting at Z, at L (set through the setting's own valueChanged, from the
# top level), and back at Z. PASS: each button does what the setting says.
import gdb

PAD = "'(anonymous namespace)::%s'"
# The default bindings (bindings.txt): A = MOUSE_LEFT, Z = TAB, L = LSHIFT.
KEY_A, KEY_Z, KEY_L = 512 + 1, 43, 225
WAIT = 0x0C400201       # MARIO_STATUS_WAIT
HIP_DROP = 0x008008A9   # MARIO_STATUS_HIP_DROP
CROUCH = 0xF00001C0     # the moveset's crouch (backflip.cpp)
SETTLE = 20             # frames standing still before each check
FRAMES = 45             # frames each check watches
MODES = {'Z': 0, 'L': 1}

# setting, button, in a jump; what must happen
CHECKS = [
    ('Z', 'L', True), ('Z', 'Z', True), ('Z', 'Z', False), ('Z', 'L', False),
    ('L', 'L', True), ('L', 'Z', True), ('L', 'L', False), ('L', 'Z', False),
    ('Z', 'L', True), ('Z', 'Z', False),
]


def ev(e):
    return gdb.parse_and_eval(e)


def member(value, *names):
    # The decomp's classes and SunshineHeaderInterface's share names (the same
    # layout, other member names); gdb may resolve either.
    for n in names:
        try:
            return value[n]
        except gdb.error:
            pass
    raise gdb.error('no member %s' % '/'.join(names))


def say(msg):
    print('crouch_button: ' + msg, flush=True)


def fail(message):
    say('error: ' + message)
    gdb.execute('kill')
    gdb.execute('quit 1')


def key(code, down):
    gdb.execute('set var %s[%d] = %d' % (PAD % 'g_key', code, 1 if down else 0), to_string=True)


def mario():
    return ev('(TMario*)gpMarioOriginal')


class Status(gdb.Breakpoint):
    # every status Mario is sent to (TMario::changePlayerStatus)
    def __init__(self):
        super().__init__('TMario::changePlayerStatus', internal=True)
        self.seen = set()

    def stop(self):
        self.seen.add(int(ev('status')) & 0xFFFFFFFF)
        return False


class Frames(gdb.Breakpoint):
    def __init__(self):
        super().__init__('TMarDirector::direct', internal=True)
        self.check = 0
        self.still = 0
        self.frame = -1
        self.mode = 'Z'
        self.meaning = 0
        self.results = []
        self.waited = 0

    def stop(self):
        self.waited += 1
        if self.waited > 40000 or self.check == len(CHECKS):
            return True
        m = mario()
        mode, button, jump = CHECKS[self.check]
        if self.frame < 0:
            if mode != self.mode:
                return True  # the setting changes from the top level
            standing = int(member(m, 'mStatus', 'mState')) & 0xFFFFFFFF == WAIT
            self.still = self.still + 1 if standing else 0
            if self.still < SETTLE:
                return False
            self.frame = 0
            status.seen = set()
            self.meaning = 0
        f = self.frame
        code = KEY_L if button == 'L' else KEY_Z
        if jump:
            if f == 0:
                key(KEY_A, True)
            elif f == 6:
                key(KEY_A, False)
            elif f == 8:
                key(code, True)
            elif f == 14:
                key(code, False)
        else:
            if f == 0:
                key(code, True)
            elif f == 20:
                key(code, False)
        if f < 20:
            pad = member(m, 'mGamePad', 'mController')
            self.meaning |= int(member(pad.dereference(), 'mMeaning')) & 0xB000
        self.frame += 1
        if self.frame == FRAMES:
            self.results.append((CHECKS[self.check], set(status.seen), self.meaning))
            self.frame = -1
            self.still = 0
            self.check += 1
        return False


gdb.execute('set confirm off')
status = Status()
frames = Frames()
gdb.execute('run')
while frames.check < len(CHECKS) and frames.waited <= 40000:
    mode = CHECKS[frames.check][0]
    s = 'gLongJumpMappingSetting'
    gdb.execute('set var %s.mLongJumpValue = %d' % (s, MODES[mode]))
    gdb.execute('call LongJumpMappingSetting::valueChanged(&%s.mLongJumpValue, &%s.mLongJumpValue, '
                '(BetterSMS::Settings::SingleSetting::ValueKind)0)' % (s, s), to_string=True)
    frames.mode = mode
    say('Long Jump/Crouch Button set to %s' % mode)
    gdb.execute('continue')
if frames.check != len(CHECKS):
    fail('stopped after %d of %d checks' % (frames.check, len(CHECKS)))

ok = True
for (mode, button, jump), seen, meaning in frames.results:
    pounds = HIP_DROP in seen
    crouches = CROUCH in seen
    if jump:
        # a first jump: L ground pounds at either setting
        want = button == 'L'
        good = pounds == want
        what = 'ground pound %s' % ('yes' if pounds else 'no')
    else:
        want = button == mode
        # the button with the L meanings centres the camera (0x8000)
        good = crouches == want and bool(meaning & 0x8000) == (not want)
        what = 'crouch %s' % ('yes' if crouches else 'no')
    say('setting %s, %s %-8s meaning %06x  %-16s %s' % (
        mode, button, 'in a jump' if jump else 'standing', meaning, what, 'ok' if good else 'WRONG'))
    ok &= good
say('PASS' if ok else 'FAILED')
gdb.execute('kill')
gdb.execute('quit %d' % (0 if ok else 1))
