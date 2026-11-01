"""get.py's Eclipse maps (the radar's maps of Super Mario Eclipse's stages):
installed from a local server into mods/maps/eclipse, checked and recorded so
--if-outdated (and SMS Launcher) installs them again only for a new release."""
import contextlib
import functools
import hashlib
import http.server
import importlib.util
import io
import json
import pathlib
import tempfile
import threading
import unittest
import zipfile

GET = pathlib.Path(__file__).resolve().parents[1] / 'get.py'
MANIFEST = b'# stage maps\n0123456789abcdef lacrima.jpg -1.0 -2.0 0.1 4 4 lacrima\n'


def load_get():
    spec = importlib.util.spec_from_file_location('sms_get', GET)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def make_zip(files):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        for name, data in files.items():
            z.writestr(name, data)
    return buf.getvalue()


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class EclipseMapsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='sms get eclipse maps ')
        root = pathlib.Path(self.tmp.name)
        self.served, self.mods = root / 'served', root / 'mods'
        self.served.mkdir()
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Quiet, directory=str(self.served)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.url = 'http://127.0.0.1:%d/' % server.server_address[1]
        self.get = load_get()
        self.get.MODS, self.get.DOWNLOADS = str(self.mods), str(self.mods / '.downloads')
        self.get.ROOT = str(root)
        self.maps = self.mods / 'maps' / 'eclipse'
        self.get.ECLIPSE_MAPS.update(install=str(self.maps))

    def tearDown(self):
        self.tmp.cleanup()

    def serve(self, name, data, md5=None):
        (self.served / name).write_bytes(data)
        self.get.ECLIPSE_MAPS.update(url=self.url + name, file=name, size=len(data),
                                     md5=md5 or hashlib.md5(data).hexdigest())

    def run_quietly(self, fn, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            fn(*args)
        return out.getvalue()

    def installed(self):
        return sorted(p.relative_to(self.maps).as_posix() for p in self.maps.rglob('*') if p.is_file())

    def test_installs_and_replaces(self):
        self.maps.mkdir(parents=True)
        (self.maps / 'old.jpg').write_bytes(b'stale')
        self.serve('maps.zip', make_zip({'eclipse/maps.txt': MANIFEST, 'eclipse/lacrima.jpg': b'jpg'}))
        out = self.run_quietly(self.get.get_eclipse_maps, False)
        self.assertEqual(self.installed(), ['.release', 'lacrima.jpg', 'maps.txt'])
        self.assertIn('Installed 1 maps in mods/maps/eclipse', out)
        self.assertFalse((self.mods / '.downloads' / 'maps.zip').exists())
        self.assertEqual(json.loads((self.maps / '.release').read_text())['md5'], self.get.ECLIPSE_MAPS['md5'])

    def test_if_outdated_installs_only_a_new_release(self):
        self.serve('v1.zip', make_zip({'eclipse/maps.txt': MANIFEST, 'eclipse/lacrima.jpg': b'one'}))
        self.assertIn('Installed 1 maps', self.run_quietly(self.get.get_eclipse_maps, False, True))
        (self.served / 'v1.zip').unlink()  # a second download would fail
        self.assertIn('The Eclipse maps are up to date', self.run_quietly(self.get.get_eclipse_maps, False, True))
        self.serve('v2.zip', make_zip({'eclipse/maps.txt': MANIFEST, 'eclipse/yoshi.jpg': b'two'}))  # a new pin
        self.assertIn('Installed 1 maps', self.run_quietly(self.get.get_eclipse_maps, False, True))
        self.assertEqual(self.installed(), ['.release', 'maps.txt', 'yoshi.jpg'])

    def test_pin_file(self):
        pin = json.loads((GET.parent / 'eclipse-maps.json').read_text())
        self.assertEqual(sorted(pin), ['file', 'md5', 'name', 'page', 'size', 'url'])
        self.assertTrue(pin['url'].endswith('/' + pin['file']))
        self.assertRegex(pin['md5'], '^[0-9a-f]{32}$')

    def test_rejects_wrong_checksum(self):
        self.serve('maps.zip', make_zip({'eclipse/maps.txt': MANIFEST}), md5='0' * 32)
        with self.assertRaises(self.get.Failure):
            self.run_quietly(self.get.get_eclipse_maps, False)
        self.assertFalse(self.maps.exists())

    def test_rejects_other_layouts(self):
        for files in ({'maps.txt': MANIFEST}, {'eclipse/lacrima.jpg': b'jpg'},
                      {'eclipse/maps.txt': MANIFEST, 'eclipse/../../x.jpg': b'jpg'}):
            self.serve('maps.zip', make_zip(files))
            with self.assertRaises(self.get.Failure):
                self.run_quietly(self.get.get_eclipse_maps, False)
            self.assertFalse(self.maps.exists())
            self.assertFalse((self.mods / 'x.jpg').exists())


if __name__ == '__main__':
    unittest.main()
