#!/usr/bin/env python3
"""Check that the Sirena hotel's rise plays its rumble (uninit-06).

Run from the port checkout; --disc accepts a GMSE01 image or extracted files
folder. Inside gdb, this script warps to Sirena Beach episode 1, puts a stale
far-away point (as a reused heap can hold) into the sink event's sound
position, and starts the hotel's rise the way the Phantamanta's defeat does
(TMapEventSirenaSink::unk64). The rise's rumble (MSD_SE_OBJ_QUAKE) must then
be requested at the point the rise's camera looks at, and JAI must be playing
it (Started, Playing, or Stopping between a held sound's requests), not storing
it as out of range.

--negative writes the stale point back after the event has set it, as the
game did before the patch: the rumble must then not play (this checks that
the check can fail).
"""
import os

try:
    import gdb
except ImportError:
    gdb = None

QUAKE = 0x3008  # MSD_SE_OBJ_QUAKE
STALE = (30000.0, 0.0, 30000.0)
# JAISoundState (JAISound.hpp)
SOUNDSTATE_STORED, SOUNDSTATE_STARTED, SOUNDSTATE_PLAYING, SOUNDSTATE_STOPPING = 1, 3, 4, 5


def install_breakpoints():
    negative = os.environ.get('HOTEL_RUMBLE_NEGATIVE') == '1'
    state = dict(visits=0, controls=0, requests=0, position=None)

    def report(message):
        print('hotel rumble: ' + message, flush=True)

    def value(expression):
        return gdb.parse_and_eval(expression)

    def quake_states():
        # The SE category list the request went to: JAIBasic -> JAIData -> mSeRegist.
        states = []
        sound = value('MSGBasic->unk0->mSeRegist[%d].mUsedHead' % ((QUAKE >> 12) & 0xFF))
        while int(sound) != 0 and len(states) < 64:
            if int(sound['mSoundID']) == QUAKE:
                states.append(int(sound['mState']))
            sound = sound['mNextSound']
        return states

    class Watch(gdb.Breakpoint):
        def stop(self):
            state['visits'] += 1
            if state['visits'] == 150:
                for axis, coordinate in zip('xyz', STALE):
                    gdb.execute('set var this->unk50[0].%s = %f' % (axis, coordinate))
                gdb.execute('set var this->unk64 = 1')
                report('rise started with a stale sound position')
            return False

    class Control(gdb.Breakpoint):
        def stop(self):
            state['controls'] += 1
            n = state['controls']
            if n == 1:
                target = value('((TMapEventSirenaSink*)this)->unk68')
                state['target'] = tuple(float(target[a]) for a in 'xyz')
                if negative:
                    for axis, coordinate in zip('xyz', STALE):
                        gdb.execute('set var this->unk50[0].%s = %f' % (axis, coordinate))
            if n == 90:
                states = quake_states()
                report('requests %d, at %s, camera target %s, JAI states %s' % (
                    state['requests'], state['position'], state.get('target'), states))
                # A held sound is Playing just after its request and Stopping
                # until the next one re-arms it; out of range it is Stored (1).
                playing = any(s in (SOUNDSTATE_STARTED, SOUNDSTATE_PLAYING, SOUNDSTATE_STOPPING)
                              for s in states)
                at_target = state['position'] == state.get('target')
                if negative:
                    ok = not playing and not at_target
                else:
                    ok = playing and at_target
                report(('PASS' if ok else 'FAIL') + (' (negative)' if negative else ''))
                return True
            return False

    class Request(gdb.Breakpoint):
        def stop(self):
            try:
                if int(value('id')) != QUAKE:
                    return False
                position = value('position')
                state['requests'] += 1
                state['position'] = tuple(float(position[a]) for a in 'xyz')
            except gdb.error:
                pass
            return False

    Watch('TMapEventSirenaSink::watch', internal=True)
    Control('TMapEventSink::control', internal=True)
    Request('MSoundSESystem::MSoundSE::startSoundActor', internal=True)


def main():
    import argparse
    import subprocess
    from concurrent.futures import ThreadPoolExecutor
    import regress

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', help='GMSE01 image or extracted files folder')
    parser.add_argument('--build32', default='build/linux-32')
    parser.add_argument('--build64', default='build/linux-64')
    parser.add_argument('--arch', default='32,64')
    parser.add_argument('--fps', default='30,60')
    parser.add_argument('--negative', action='store_true',
                        help='restore the stale position after the event sets it; expects no rumble')
    parser.add_argument('--work', default='build/hotel-rumble-regress')
    parser.add_argument('--timeout', type=int, default=400)
    args = parser.parse_args()
    disc = args.disc or regress.find_disc()
    if not disc or not os.path.exists(disc):
        parser.error('supply a GMSE01 image or extracted files folder with --disc')
    disc = os.path.abspath(disc)
    archs, rates = args.arch.split(','), args.fps.split(',')
    if any(a not in ('32', '64') for a in archs) or any(f not in ('30', '60', '120') for f in rates):
        parser.error('--arch takes 32,64; --fps takes 30,60,120')
    executables = {a: os.path.abspath(getattr(args, 'build' + a) + '/sms') for a in archs}
    if any(not os.path.isfile(exe) for exe in executables.values()):
        parser.error('build each requested executable first')

    def run(arch, fps):
        directory = os.path.abspath(os.path.join(args.work, '%s-%s' % (arch, fps)))
        os.makedirs(directory, exist_ok=True)
        save = os.path.join(directory, 'save')
        os.makedirs(save, exist_ok=True)
        env = regress.base_env(save)
        env.update(SMS_WARP='6,0', SMS_FRAME_RATE=fps, SMS_AUTOPRESS=regress.GATE_AP,
                   HOTEL_RUMBLE_NEGATIVE='1' if args.negative else '0')
        command = regress.GDB + ['-x', os.path.abspath(__file__), '-ex', 'run',
                                 '--args', executables[arch], disc]
        log_path = os.path.join(directory, 'run.log')
        with open(log_path, 'wb') as log:
            process = subprocess.Popen(command, cwd=regress.ROOT, env=env,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL, start_new_session=True)
            try:
                process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                regress.kill_group(process)
        with open(log_path, errors='replace') as log:
            contents = log.read()
        passed = ('hotel rumble: PASS' in contents and 'hotel rumble: FAIL' not in contents
                  and 'Fatal signal:' not in contents)
        detail = next((line for line in contents.splitlines() if line.startswith('hotel rumble: requests')),
                      'see ' + log_path)
        print('%s hotel rumble %s-bit %s fps%s: %s' %
              ('PASS' if passed else 'FAIL', arch, fps, ' (negative)' if args.negative else '', detail),
              flush=True)
        return passed

    with ThreadPoolExecutor(max_workers=1) as pool:
        results = list(pool.map(lambda pair: run(*pair),
                                [(a, f) for f in rates for a in archs]))
    return 0 if all(results) else 1


if gdb is not None:
    install_breakpoints()
elif __name__ == '__main__':
    raise SystemExit(main())
