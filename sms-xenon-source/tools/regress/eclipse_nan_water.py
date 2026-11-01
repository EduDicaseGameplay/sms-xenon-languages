# Water with a NaN speed in an Eclipse stage, under gdb (docs/DEVELOPMENT.md).
#
# A water particle sent off with a NaN speed (as normalising a zero vector
# gives) has a NaN position from its first move, and BetterSunshineEngine's
# wall check (checkWallsExotic_, in place of the game's at the particle's wall
# test) turned it into the cell number 0x80000000. A console reads cell 0
# there, its address wrapping around 32 bits; a 64-bit host faulted.
#
# Run on an Eclipse build with the Eclipse disc, the card regress.py's
# ecl-firstboot run saves, SMS_WARP=66,1 (Hotel Lacrima's basement, the
# Tramplin' Stu's episodes) and regress.py's ECL_WARP_AP input. Once the Stu
# moves, 50 particles with a NaN speed are emitted above Mario (the water
# manager drops particles far from him before they move) with the Stu's own
# emit info, from the top level (nothing is called from a breakpoint's
# stop()); PASS: the game runs NAN_SECONDS (default 20) more without a fault.
import os

import gdb

SECONDS = float(os.environ.get('NAN_SECONDS', '20'))
# Emitted this high above Mario: clear of his and the Stu's hit actors, which
# would take the particles before their first wall check.
HIGH = 300.0
signal = []


def say(msg):
    print('nan-water: ' + msg, flush=True)


def ev(expr):
    return gdb.parse_and_eval(expr)


def on_stop(event):
    if isinstance(event, gdb.SignalEvent):
        signal.append(event.stop_signal)


class Stu(gdb.Breakpoint):
    """THinokuri2::moveObject of the first Stu: stops there once, then counts."""

    def __init__(self):
        super().__init__('THinokuri2::moveObject')
        self.stu = None
        self.frames = 0
        self.limit = None

    def stop(self):
        this = int(ev('(uintptr_t)this'))
        if self.stu is None:
            self.stu = this
            return True
        if this != self.stu:
            return False
        self.frames += 1
        return self.limit is not None and self.frames >= self.limit


def fail(message):
    say('error: ' + message)
    gdb.execute('bt 12')
    gdb.execute('kill')
    gdb.execute('quit 1')


gdb.events.stop.connect(on_stop)
gdb.execute('set confirm off')
stu = Stu()
gdb.execute('run')
if signal or stu.stu is None:
    fail('stopped before the Stu moved (%s)' % (signal or 'no Stu'))

info = '((THinokuri2*)%d)->unk19C' % stu.stu
# Mario's position: JDrama::TPlacement is declared twice in an Eclipse binary
# (the decomp's mPosition, SunshineHeaderInterface's mTranslation).
pos = None
for name in ('mPosition', 'mTranslation'):
    try:
        pos = ev('((TMario*)gpMarioOriginal)->%s' % name)
        break
    except gdb.error:
        pass
if pos is None:
    fail("no position for Mario")
for axis in 'xyz':
    gdb.execute('set var %s->mPos.value.%s = %.9g' % (info, axis, float(pos[axis]) + (HIGH if axis == 'y' else 0)))
    gdb.execute('set var *(unsigned int*)&%s->mV.value.%s = 0x7fc00000' % (info, axis))
num = int(ev('%s->mNum.value' % info))
gdb.execute('set var %s->mNum.value = 50' % info)
emitted = int(ev('(int)gpModelWaterManager->emitRequest(*%s)' % info))
gdb.execute('set var %s->mNum.value = %d' % (info, num))
if emitted == 0:
    fail('no particle was emitted')
say('%d particles with a NaN speed at %.0f %.0f %.0f' % (emitted, float(pos['x']), float(pos['y']) + HIGH, float(pos['z'])))

stu.limit = int(SECONDS * int(ev('port_frame_rate')))
gdb.execute('continue')
if signal:
    fail('%s %d Stu frames after the NaN water' % (signal[0], stu.frames))
say('PASS: %d Stu frames after the NaN water' % stu.frames)
gdb.execute('kill')
gdb.execute('quit 0')
