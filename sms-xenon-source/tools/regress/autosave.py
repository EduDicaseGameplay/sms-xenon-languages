# Auto save under gdb (SMS_AUTO_SAVE=1, a card whose file A exists, SMS_WARP=2,7,1):
#   a Turbo nozzle box opened for good (save prompt kind 4),
#   a blue coin's prompt (kind 1, raised as fireGetBlueCoin does),
#   a Shine (kind 6: one in the stage made to appear, Mario put on it).
# Each must save (the save count and last save time move on, the saved copy
# of the flags is taken) without the director entering the prompt (state 11),
# and the Shine must still leave the stage. AUTOSAVE_PART=shine: only the Shine.
# Nothing is called in the game from inside a breakpoint's stop().
import gdb
import os


def say(msg):
    print('autosave: ' + msg, flush=True)


def fail(message):
    say('error: ' + message)
    gdb.execute('bt 12')
    gdb.execute('quit 1')


def ev(expr):
    return gdb.parse_and_eval(expr)


DIR = '((TMarDirector*)gpMarDirector)'
MARIO = '((TMario*)gpMarioOriginal)'
FLAGS = '((TFlagManager*)TFlagManager::smInstance)'


def saves():
    return (int(ev(FLAGS + '->getFlag(0x20000)')), int(ev(FLAGS + '->mLastSaveTime')),
            int(ev(FLAGS + '->mSavedLastSaveTime')))


def check_saved(before, what):
    after = saves()
    if after[0] <= before[0]:
        fail('%s: the save count did not move on (%d -> %d)' % (what, before[0], after[0]))
    if after[1] == before[1]:
        fail('%s: the last save time did not move on' % what)
    if after[2] != after[1]:
        fail('%s: the saved copy was not taken (saveSuccess)' % what)
    say('%s: saved (count %d -> %d)' % (what, before[0], after[0]))
    return after


class Ready(gdb.Breakpoint):
    box = None

    def stop(self):
        if int(ev(DIR + '->mState')) != 4:
            return False
        box = ev('this')
        if int(box['mContainedNozzleType']) != 5 or not bool(box['unk15C']):
            return False
        self.box = box
        return True


class Watch(gdb.Breakpoint):
    """Each director tick: notes a prompt; optionally keeps Mario on a point."""

    def __init__(self):
        super().__init__('TMarDirector::direct')
        self.frames = self.limit = 0
        self.prompt = False
        self.follow = self.until = None

    def stop(self):
        self.frames += 1
        if int(ev(DIR + '->mState')) == 11:
            self.prompt = True
        if self.follow is not None:
            for axis, v in zip('xyz', self.follow()):
                gdb.execute('set var %s->mPosition.%s = %.9g' % (MARIO, axis, v))
        if self.until is not None and self.until():
            return True
        return self.frames >= self.limit

    def run(self, seconds, follow=None, until=None):
        self.frames = 0
        self.limit = int(seconds * int(ev('port_frame_rate')))
        self.follow, self.until = follow, until
        gdb.execute('continue')


class FindShine(gdb.Breakpoint):
    found = None

    def stop(self):
        self.found = int(ev('(uintptr_t)this'))
        return True


class Left(gdb.Breakpoint):
    hit = False

    def stop(self):
        self.hit = True
        return False


def nozzle_part(watch, box, area, before):
    nozzle = box['mContainedNozzleItem']
    gdb.execute('call ((TNozzleBox*)%d)->receiveMessage((THitActor*)gpMarioOriginal, 0)' % int(box))
    watch.run(10, follow=lambda: tuple(float(nozzle['mPosition'][a]) for a in 'xyz'),
              until=lambda: int(ev(MARIO + '->mWaterGun->mCurrentNozzle')) == 5)
    watch.run(3)
    if not bool(ev(FLAGS + '->getNozzleRight(%d, 1)' % area)):
        fail('the nozzle was not unlocked')
    if watch.prompt:
        fail('nozzle: the save prompt opened')
    return check_saved(before, 'nozzle')


def blue_coin_part(watch, before):
    gdb.execute('set var %s->unk4C = %s->unk4C | 0x200' % (DIR, DIR))
    gdb.execute('set var %s->unk261 = 1' % DIR)
    watch.run(3)
    if watch.prompt:
        fail('blue coin: the save prompt opened')
    before = check_saved(before, 'blue coin')
    watch.run(1.5)  # to the indicator's Saved, and its sound
    return before


def shine_part(watch, before):
    watch.enabled = False
    find = FindShine('TShine::perform')
    gdb.execute('continue')
    find.delete()
    watch.enabled = True
    shine = find.found
    gdb.execute('call ((TShine*)%d)->appearSimple(0)' % shine)
    say('Shine %#x' % shine)
    watch.run(10, follow=lambda: tuple(float(ev('((TShine*)%d)->mPosition.%s' % (shine, a))) for a in 'xyz'),
              until=lambda: int(ev(DIR + '->unk4C')) & 0x8001)
    if not int(ev(DIR + '->unk4C')) & 0x8001:
        fail('Mario did not get the Shine (flags %#x)' % int(ev(DIR + '->unk4C')))
    say('Shine got')
    left = Left('TMarDirector::moveStage')
    watch.run(25, until=lambda: left.hit)
    if watch.prompt:
        fail('Shine: the save prompt opened')
    if not left.hit:
        fail('Shine: the stage did not move on (state %d)' % int(ev(DIR + '->mState')))
    return check_saved(before, 'Shine')


gdb.execute('set confirm off')
ready = Ready('TNozzleBox::control')
gdb.execute('run')
if ready.box is None:
    fail('stopped before reaching a valid Turbo box')
ready.delete()
area = int(ev('((TApplication*)&gpApplication)->mCurrArea.unk0'))
before = saves()
say('stage %d, file %d, saves so far %d' % (area, int(ev('((TApplication*)&gpApplication)->mSaveFile')), before[0]))
watch = Watch()
if os.environ.get('AUTOSAVE_PART') == 'blue':
    say('GX frame at the blue coin: %d' % int(ev('GXPC_FrameCount()')))
    before = blue_coin_part(watch, before)
    watch.run(3)
    say('GX frame after: %d' % int(ev('GXPC_FrameCount()')))
    say('PASS')
    gdb.execute('kill')
    gdb.execute('quit 0')
if os.environ.get('AUTOSAVE_PART') == 'fallback':
    # a slot another game saved over since: the prompt must ask
    before = blue_coin_part(watch, before)
    gdb.execute('set var %s->mLastSaveTime = 12345' % FLAGS)
    gdb.execute('set var %s->unk4C = %s->unk4C | 0x200' % (DIR, DIR))
    gdb.execute('set var %s->unk261 = 1' % DIR)
    watch.run(3)
    if not watch.prompt:
        fail('fallback: the prompt did not open')
    say('fallback: the prompt opened')
    say('PASS')
    gdb.execute('kill')
    gdb.execute('quit 0')
if os.environ.get('AUTOSAVE_PART') != 'shine':
    before = nozzle_part(watch, ready.box, area, before)
    before = blue_coin_part(watch, before)
before = shine_part(watch, before)
say('PASS')
gdb.execute('kill')
gdb.execute('quit 0')
