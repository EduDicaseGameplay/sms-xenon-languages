"""get.py's Eclipse HD textures: the textures unpacked as they arrive with HTTP
range requests (no 9 GB zip on disk), checked against the pinned index and
each texture's CRC-32, carried on after a stopped install, and recorded so
--if-outdated (and SMS Launcher) installs them again only for a new release."""
import contextlib
import hashlib
import http.server
import importlib.util
import io
import json
import pathlib
import re
import tempfile
import threading
import unittest
import zipfile

GET = pathlib.Path(__file__).resolve().parents[1] / 'get.py'


def load_get():
    spec = importlib.util.spec_from_file_location('sms_get', GET)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Unseekable(io.RawIOBase):
    """Makes zipfile write data descriptors after each entry, as zips made by streaming have."""
    def __init__(self):
        self.buf = io.BytesIO()

    def writable(self):
        return True

    def write(self, b):
        return self.buf.write(b)


def make_zip(files, stream=False):
    out = Unseekable() if stream else io.BytesIO()
    with zipfile.ZipFile(out, 'w') as z:
        for name, (data, method) in files.items():
            if name.endswith('/'):
                z.writestr(name, b'')
            else:
                z.writestr(zipfile.ZipInfo(name, (2026, 10, 27, 0, 0, 0)), data, compress_type=method)
    return (out.buf if stream else out).getvalue()


def texture(n):
    return bytes((n * 7 + k) % 251 for k in range(3000 + n * 500))


PACK = {
    'HowToInstall.txt': (b'copy GMSE04 into Load/Textures', zipfile.ZIP_DEFLATED),
    'GMSE04/': (b'', 0),
    'GMSE04/scene/': (b'', 0),
    'GMSE04/scene/tex1_64x64_aaaa_14.dds': (texture(1), zipfile.ZIP_DEFLATED),
    'GMSE04/scene/tex1_32x32_bbbb_14.dds': (texture(2), zipfile.ZIP_STORED),
    'GMSE04/common/tex1_16x16_cccc_14.dds': (texture(3), zipfile.ZIP_DEFLATED),
    'GMSE04/common/notes.txt': (b'not a texture', zipfile.ZIP_DEFLATED),
    'GMSE04/title/tex1_128x64_dddd_14.png': (texture(4), zipfile.ZIP_DEFLATED),
}
TEXTURES = ['GMSE04/common/tex1_16x16_cccc_14.dds', 'GMSE04/scene/tex1_32x32_bbbb_14.dds',
            'GMSE04/scene/tex1_64x64_aaaa_14.dds', 'GMSE04/title/tex1_128x64_dddd_14.png']


class Ranges(http.server.BaseHTTPRequestHandler):
    """Serves server.files with Range requests; server.ranges=False ignores them, and
    server.cut_after stops one response after that many bytes."""
    def log_message(self, *args):
        pass

    def do_GET(self):
        data = self.server.files.get(self.path.lstrip('/'))
        if data is None:
            self.send_error(404)
            return
        m = re.fullmatch(r'bytes=(\d+)-(\d+)', self.headers.get('Range') or '')
        if not m or not self.server.ranges:
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        start, end = int(m.group(1)), min(int(m.group(2)), len(data) - 1)
        body = data[start:end + 1]
        self.server.sent += len(body)
        self.send_response(206)
        self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, len(data)))
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        cut = self.server.cut_after
        if cut is not None and len(body) > cut:
            self.server.cut_after = None
            self.wfile.write(body[:cut])
            self.wfile.flush()
            self.close_connection = True
            return
        self.wfile.write(body)


class EclipseTexturesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='sms get eclipse textures ')
        root = pathlib.Path(self.tmp.name)
        self.mods = root / 'mods'
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Ranges)
        self.server.files, self.server.ranges, self.server.cut_after, self.server.sent = {}, True, None, 0
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.url = 'http://127.0.0.1:%d/' % self.server.server_address[1]
        self.get = load_get()
        self.get.MODS, self.get.DOWNLOADS, self.get.ROOT = str(self.mods), str(self.mods / '.downloads'), str(root)
        self.get.RUN = 4000  # several runs, even for these small textures
        self.get.time.sleep = lambda s: None
        self.pack = self.mods / 'eclipse-textures'
        self.get.ECLIPSE_TEXTURES.update(install=str(self.pack))

    def tearDown(self):
        self.tmp.cleanup()

    def serve(self, name, data, fallback=None, **pin):
        self.server.files[name] = data
        start = zipfile.ZipFile(io.BytesIO(data)).start_dir
        self.get.ECLIPSE_TEXTURES.update(url=self.url + name, file=name, size=len(data),
                                         md5=hashlib.md5(data).hexdigest(),
                                         index_md5=hashlib.md5(data[start:]).hexdigest(),
                                         fallback_urls=[self.url + fallback] if fallback else [])
        self.get.ECLIPSE_TEXTURES.update(pin)

    def run_quietly(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.get.get_eclipse_textures(*args)
        return out.getvalue()

    def installed(self, folder=None):
        folder = folder or self.pack
        return sorted(p.relative_to(folder).as_posix() for p in folder.rglob('*') if p.is_file())

    def assert_pack(self):
        self.assertEqual(self.installed(), ['.release'] + TEXTURES)
        for name in TEXTURES:
            self.assertEqual((self.pack / name).read_bytes(), PACK[name][0])
        self.assertEqual(json.loads((self.pack / '.release').read_text())['md5'], self.get.ECLIPSE_TEXTURES['md5'])
        self.assertFalse((self.mods / 'eclipse-textures.part').exists())

    def test_installs_textures_only(self):
        self.serve('pack.zip', make_zip(PACK))
        out = self.run_quietly()
        self.assert_pack()
        self.assertIn('Installed 4 textures in mods/eclipse-textures', out)
        self.assertRegex(out, r'Eclipse texture download: (\d+)/\1 bytes \(4/4 textures\)')
        self.assertFalse((self.mods / '.downloads').exists())  # no zip kept anywhere

    def test_data_descriptors(self):
        self.serve('pack.zip', make_zip(PACK, stream=True))
        self.run_quietly()
        self.assert_pack()

    def test_if_outdated_installs_only_a_new_release(self):
        self.serve('v1.zip', make_zip(PACK))
        self.run_quietly(True)
        del self.server.files['v1.zip']  # a second download would fail
        self.assertIn('The Eclipse HD textures are up to date', self.run_quietly(True))
        other = dict(PACK, **{'GMSE04/scene/tex1_8x8_eeee_14.dds': (texture(5), zipfile.ZIP_DEFLATED)})
        self.serve('v2.zip', make_zip(other))  # a new pin
        self.assertIn('Installed 5 textures', self.run_quietly(True))

    def test_carries_on_after_a_stopped_install(self):
        data = make_zip(PACK)
        self.serve('pack.zip', data)
        self.get.RUN_TRIES = 1
        self.server.ranges = True
        texture_starts = [i.header_offset for i in zipfile.ZipFile(io.BytesIO(data)).infolist()
                          if i.filename in TEXTURES]
        # The index reads come first; then the second run of textures breaks once
        # and the installer gives up (one try), keeping the first run's textures.
        original = self.get.unpack_run
        calls = []

        def unpack_run(url, run, *rest):
            calls.append([i.filename for i in run])
            if len(calls) == 2:
                raise OSError('connection reset')
            return original(url, run, *rest)

        self.get.unpack_run = unpack_run
        with self.assertRaises(self.get.Failure) as stopped:
            self.run_quietly()
        self.assertIn('kept for the next try', str(stopped.exception))
        staged = self.mods / 'eclipse-textures.part'
        first = calls[0]
        self.assertEqual(self.installed(staged), sorted(['.done'] + first))
        self.assertFalse(self.pack.exists())
        # The next install fetches only the rest.
        self.get.unpack_run, calls[:] = original, []
        self.get.RUN_TRIES = 5
        self.server.sent = 0
        out = self.run_quietly()
        self.assertIn('carrying on: %d of 4 textures were installed before' % len(first), out)
        self.assert_pack()
        fetched = max(texture_starts) - min(texture_starts)
        self.assertLess(self.server.sent, len(data) - 1)  # not everything again
        self.assertGreater(fetched, 0)

    def test_a_dropped_connection_is_tried_again(self):
        self.serve('pack.zip', make_zip(PACK))
        self.get.RUN = 1 << 20  # one run, which breaks once partway
        original = self.get.http_range

        def http_range(url, start, end):
            if end - start > 2000 and self.server.cut_after is None and not getattr(self, 'cut', False):
                self.cut = True
                self.server.cut_after = 5000
            return original(url, start, end)

        self.get.http_range = http_range
        out = self.run_quietly()
        self.assertIn('the download stopped', out)
        self.assert_pack()

    def test_falls_back_when_ranges_are_not_sent(self):
        data = make_zip(PACK)
        self.server.files['mirror.zip'] = data
        self.serve('fallback.zip', data)
        self.get.ECLIPSE_TEXTURES.update(url=self.url + 'nowhere.zip', fallback_urls=[self.url + 'fallback.zip'])
        out = self.run_quietly()
        self.assertIn('failed: HTTP Error 404', out)
        self.assert_pack()
        self.server.ranges = False
        self.get.ECLIPSE_TEXTURES.update(md5='0' * 32)  # not current: install again
        with self.assertRaises(self.get.Failure) as e:
            self.run_quietly()
        self.assertIn('does not send parts of the file', str(e.exception))

    def test_rejects_another_release(self):
        for pin in ({'index_md5': '0' * 32}, {'size': 12345}):
            self.serve('pack.zip', make_zip(PACK), **pin)
            with self.assertRaises(self.get.Failure):
                self.run_quietly()
            self.assertFalse(self.pack.exists())

    def test_rejects_a_damaged_texture(self):
        data = bytearray(make_zip(PACK))
        info = zipfile.ZipFile(io.BytesIO(bytes(data))).getinfo('GMSE04/scene/tex1_32x32_bbbb_14.dds')  # stored
        at = info.header_offset + 30 + len(info.filename) + 100
        data[at] ^= 0xFF
        self.serve('pack.zip', bytes(data))
        self.get.RUN_TRIES = 2
        with self.assertRaises(self.get.Failure):
            self.run_quietly()
        self.assertFalse(self.pack.exists())
        self.assertFalse((self.mods / 'eclipse-textures.part' / 'GMSE04' / 'scene' / 'tex1_32x32_bbbb_14.dds').exists())

    def test_rejects_other_layouts(self):
        for files in ({'Textures/tex1_1x1_a_14.dds': (b'x', zipfile.ZIP_STORED)},
                      {'GMSE04/../evil/tex1_1x1_a_14.dds': (b'x', zipfile.ZIP_STORED)}):
            self.serve('pack.zip', make_zip(files))
            with self.assertRaises(self.get.Failure):
                self.run_quietly()
            self.assertFalse(self.pack.exists())
            self.assertFalse((self.mods / 'evil').exists())

    def test_pin_file(self):
        pin = json.loads((GET.parent / 'eclipse-textures.json').read_text())
        self.assertLessEqual({'file', 'index_md5', 'md5', 'name', 'page', 'size', 'unpacked', 'url'}, set(pin))
        self.assertLessEqual(set(pin), {'file', 'index_md5', 'md5', 'name', 'page', 'size', 'unpacked', 'url',
                                        'fallback_urls'})
        for key in ('md5', 'index_md5'):
            self.assertRegex(pin[key], '^[0-9a-f]{32}$')
        self.assertGreater(pin['unpacked'], pin['size'])


if __name__ == '__main__':
    unittest.main()
