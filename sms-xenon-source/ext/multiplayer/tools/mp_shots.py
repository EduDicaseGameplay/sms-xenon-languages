#!/usr/bin/env python3
"""Multiplayer screenshots: one hosting game, headless and scripted into
Delfino Plaza, with test bots (mp_bot) as the other players, and captures
of the host's screen converted to PNG.

usage: ext/multiplayer/tools/mp_shots.py --disc GMSE01.iso [--bots 3]
           [--shots 3800,4400,...] [--out DIR] [--build build/linux-64]
           [--autopress SPEC] [--warp 1,0,1] [--seconds 300] [--client]

--client also runs a second game that joins (instead of the first bot), and
captures its screen too (its captures go to OUT/client).
The game runs under a 2.5 GB memory cap (systemd-run), like the other test
tools on a small machine."""
import argparse, os, random, shutil, signal, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
PLAZA_AP = 'START@1400,STICK_LEFT@2000,A@2400,STICK_LEFT@3100,A@3250,A@3450,A@3800,STICK_UP@4400+60,B@4800'


WINDOW = False
EXTRA_ENV = {}


def frames(spec):
    """'100,200-210,300-400/2' -> '100,200,201,...'"""
    out = []
    for part in spec.split(','):
        part = part.strip()
        if not part:
            continue
        step = 1
        if '/' in part:
            part, step = part.split('/')
            step = int(step)
        if '-' in part:
            a, b = part.split('-')
            out += list(range(int(a), int(b) + 1, step))
        else:
            out.append(int(part))
    return out


def jpeg_converter(dirs, stop):
    """Turns finished captures into JPEGs as they come (long video captures)."""
    from PIL import Image
    while True:
        done = stop.is_set()
        for d in dirs:
            if not os.path.isdir(d):
                continue
            now = time.time()
            for f in sorted(os.listdir(d)):
                if not f.endswith('.ppm'):
                    continue
                path = os.path.join(d, f)
                try:
                    if not done and now - os.path.getmtime(path) < 1.0:
                        continue  # still being written
                    Image.open(path).convert('RGB').save(path[:-4] + '.jpg', quality=92)
                    os.remove(path)
                except OSError:
                    pass
        if done:
            return
        time.sleep(0.5)


def game_env(save_dir, shot_dir, shots, extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('SMS_')}
    env.update({
        'SMS_SETTINGS': os.path.join(ROOT, 'tools', 'regress', 'empty-settings.txt'),
        'SMS_TEXTURE_PACKS': '0', 'SMS_FRAME_RATE': '30', 'SMS_AUDIO': '0',
        'SMS_SKIP_MOVIES': '1', 'SMS_QUIET_STUBS': '1', 'SMS_SAVE_DIR': save_dir, 'SMS_SHOT_DIR': shot_dir,
    })
    if WINDOW:
        env.update({'SDL_VIDEODRIVER': 'offscreen', 'SMS_WINDOW_SHOTS': shots})
    else:
        env.update({'SMS_HEADLESS': '1', 'SMS_SHOTS': shots})
    env.update(extra)
    env.update(EXTRA_ENV)
    return env


def capped(cmd, mem='2500M'):
    if shutil.which('systemd-run'):
        return ['systemd-run', '--user', '--scope', '-q', '-p', 'MemoryMax=' + mem, '-p', 'MemorySwapMax=0', '--'] + cmd
    return cmd


def to_png(raw, out):
    from PIL import Image
    os.makedirs(out, exist_ok=True)
    made = []
    for f in sorted(os.listdir(raw)):
        if f.endswith('.ppm'):
            png = os.path.join(out, f[:-4] + '.png')
            Image.open(os.path.join(raw, f)).convert('RGB').save(png)
            made.append(png)
    return made


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disc', required=True)
    ap.add_argument('--build', default=os.path.join(ROOT, 'build', 'linux-64'))
    ap.add_argument('--bots', type=int, default=3)
    ap.add_argument('--shots', default='3800,4300,4500,4700,4900,5200')
    ap.add_argument('--autopress', default=PLAZA_AP)
    ap.add_argument('--warp', default='1,0,1')
    ap.add_argument('--seconds', type=float, default=300)
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'multiplayer-shots'))
    ap.add_argument('--client', action='store_true')
    ap.add_argument('--client-shots', default=None)
    ap.add_argument('--client-autopress', default=None)
    ap.add_argument('--turn', action='store_true', help='bots face the camera')
    ap.add_argument('--test', default=None, help='SMS_MULTIPLAYER_TEST for the host (take@N:K;kill@N:K;die@N:L;warp@N:area,episode)')
    ap.add_argument('--client-test', default=None, help='SMS_MULTIPLAYER_TEST for the client')
    ap.add_argument('--realtime', action='store_true',
                    help='games paced by the wall clock, as players play (else as fast as they run: '
                         'repeatable, but game time then outruns the network\'s real time)')
    ap.add_argument('--host-env', action='append', default=[],
                    help='KEY=VALUE for the host only, over the rest (e.g. its port behind a room)')
    ap.add_argument('--client-env', action='append', default=[],
                    help='KEY=VALUE for the client only, over the rest (e.g. a relay link\'s address)')
    ap.add_argument('--bot-args', default='', help='extra mp_bot arguments for each bot, ;-separated per bot')
    ap.add_argument('--env', action='append', default=[], help='KEY=VALUE for the games (repeatable)')
    ap.add_argument('--jpeg', action='store_true', help='keep captures as JPEG (converted while the games run)')
    ap.add_argument('--bot-join', default='', help='seconds after the host opens its first stage that each bot joins, ;-separated')
    ap.add_argument('--late', type=float, default=0, help='start the last bot this many seconds after the others')
    ap.add_argument('--client-after', default='', help='start the client once the host\'s log has this text (e.g. a test aid\'s line)')
    ap.add_argument('--client-delay', type=float, default=0, help='and this many seconds after that (a player who joins late)')
    ap.add_argument('--window', action='store_true',
                    help='capture the window as shown (notices and player list included) instead of the picture, '
                         'through SDL\'s offscreen driver; --shots are then GX frames')
    ap.add_argument('--offsets', default='130,-60;-130,-60;0,-170;260,-120;-260,-120;130,-280;-130,-280', help='bots\' places beside the host, X,Z;...')
    args = ap.parse_args()
    global WINDOW
    WINDOW = args.window
    for item in args.env:
        key, _, value = item.partition('=')
        EXTRA_ENV[key] = value
    args.shots = ','.join(str(n) for n in frames(args.shots))
    if args.client_shots:
        args.client_shots = ','.join(str(n) for n in frames(args.client_shots))

    port = str(30000 + random.randrange(20000))
    work = tempfile.mkdtemp(prefix='mp-shots-')
    bot = os.path.join(ROOT, 'build-multiplayer-test', 'mp_bot')
    if args.bots and not os.path.exists(bot):
        subprocess.check_call(['make', '-C', HERE])
    procs = []
    stop = threading.Event()
    converter = None
    if args.jpeg:
        converter = threading.Thread(target=jpeg_converter,
                                     args=([os.path.join(work, 'host-raw'), os.path.join(work, 'client-raw')], stop))
        converter.start()
    try:
        raw = os.path.join(work, 'host-raw')
        os.makedirs(raw)
        host_save = os.path.join(work, 'host-save')
        os.makedirs(host_save)
        env = game_env(host_save, raw, args.shots, {
            'SMS_MULTIPLAYER': 'host', 'SMS_MULTIPLAYER_PORT': port, 'SMS_MULTIPLAYER_NAME': 'Host',
            'SMS_WARP': args.warp, 'SMS_AUTOPRESS': args.autopress, 'SMS_VI_DETERMINISTIC': '0' if args.realtime else '1',
            'SMS_MULTIPLAYER_DEBUG': '1', **({'SMS_MULTIPLAYER_TEST': args.test} if args.test else {})})
        env.update(dict(kv.split('=', 1) for kv in args.host_env))
        host_log = open(os.path.join(work, 'host.log'), 'w')
        host = subprocess.Popen(capped([os.path.join(args.build, 'sms'), args.disc]), cwd=args.build, env=env,
                                stdout=host_log, stderr=subprocess.STDOUT, start_new_session=True)
        procs.append(host)
        time.sleep(3)
        client = None
        names = ['Luigi', 'Wario', 'Metal', 'Fire', 'Purple', 'Orange', 'Ice']
        offsets = args.offsets.split(';')
        first_bot = 0
        if args.client:
            while args.client_after and host.poll() is None:
                with open(os.path.join(work, 'host.log'), errors='replace') as f:
                    if args.client_after in f.read():
                        break
                time.sleep(0.05)
            time.sleep(args.client_delay)
            craw = os.path.join(work, 'client-raw')
            os.makedirs(craw)
            csave = os.path.join(work, 'client-save')
            os.makedirs(csave)
            cenv = game_env(csave, craw, args.client_shots or args.shots, {
                'SMS_MULTIPLAYER': 'join', 'SMS_MULTIPLAYER_ADDRESS': '127.0.0.1:' + port,
                'SMS_MULTIPLAYER_NAME': 'Client', 'SMS_VI_DETERMINISTIC': '0' if args.realtime else '1', 'SMS_MULTIPLAYER_DEBUG': '1',
                **({'SMS_AUTOPRESS': args.client_autopress} if args.client_autopress else {}),
                **({'SMS_MULTIPLAYER_TEST': args.client_test} if args.client_test else {})})
            cenv.update(dict(kv.split('=', 1) for kv in args.client_env))
            client_log = open(os.path.join(work, 'client.log'), 'w')
            client = subprocess.Popen(capped([os.path.join(args.build, 'sms'), args.disc]), cwd=args.build,
                                      env=cenv, stdout=client_log, stderr=subprocess.STDOUT, start_new_session=True)
            procs.append(client)
            first_bot = 1
            time.sleep(6)  # the client joins first: it is player 2
        joins = [x for x in args.bot_join.split(';')] if args.bot_join else []
        session_at = None
        for i in range(first_bot, args.bots):
            if args.late and i == args.bots - 1:
                time.sleep(args.late)
            if i - first_bot < len(joins) and joins[i - first_bot].strip():
                # wait for the host's first stage, then this long
                while session_at is None and host.poll() is None:
                    with open(os.path.join(work, 'host.log')) as f:
                        if 'session 1:' in f.read():
                            session_at = time.time()
                    time.sleep(0.2)
                wait = (session_at or time.time()) + float(joins[i - first_bot]) - time.time()
                if wait > 0:
                    time.sleep(wait)
            log = open(os.path.join(work, 'bot%d.log' % i), 'w')
            benv = dict(os.environ, MP_BOT_DEBUG='1', SMS_MULTIPLAYER_DEBUG='1')
            build_file = os.path.join(args.build, 'multiplayer-build.txt')
            if os.path.exists(build_file):
                benv['MP_BOT_BUILD'] = open(build_file).read().strip()
            procs.append(subprocess.Popen([bot, '127.0.0.1:' + port, '--name', names[i % len(names)], '--offset', offsets[i % len(offsets)],
                                           '--seconds', str(args.seconds), '--coins', '3' if i == first_bot else '0']
                                          + (['--turn'] if args.turn else [])
                                          + (args.bot_args.split(';')[i].split() if i < len(args.bot_args.split(';')) else []),
                                          stdout=log, stderr=subprocess.STDOUT, start_new_session=True, env=benv))
            time.sleep(0.5)
        deadline = time.time() + args.seconds
        while time.time() < deadline and host.poll() is None:
            # done once the last capture is written
            name = 'window%05d' if args.window else 'field%05d'
            have = lambda d, n: any(os.path.exists(os.path.join(d, (name % n) + ext)) for ext in ('.ppm', '.jpg'))
            last = max(int(x) for x in args.shots.split(','))
            if have(raw, last) and (not client or have(os.path.join(work, 'client-raw'), max(
                    int(x) for x in (args.client_shots or args.shots).split(',')))):
                time.sleep(1)
                break
            time.sleep(1)
    finally:
        for p in procs:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        time.sleep(1)
        for p in procs:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.wait()
    if converter:
        stop.set()
        converter.join()
        for side in ('host-raw', 'client-raw'):
            src = os.path.join(work, side)
            if os.path.isdir(src):
                dst = os.path.join(args.out, 'host' if side == 'host-raw' else 'client')
                os.makedirs(dst, exist_ok=True)
                for f in os.listdir(src):
                    shutil.move(os.path.join(src, f), os.path.join(dst, f))
    os.makedirs(args.out, exist_ok=True)
    made = to_png(os.path.join(work, 'host-raw'), args.out)
    if args.client:
        made += to_png(os.path.join(work, 'client-raw'), os.path.join(args.out, 'client'))
    for f in os.listdir(work):
        if f.endswith('.log'):
            shutil.copy(os.path.join(work, f), os.path.join(args.out, f))
    shutil.rmtree(work, ignore_errors=True)
    for m in made:
        print(m)
    print('logs in', args.out)


if __name__ == '__main__':
    main()
