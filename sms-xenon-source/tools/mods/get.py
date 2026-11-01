#!/usr/bin/env python3
"""Download and install optional mods, fresh each time.

    python3 tools/mods/get.py textures    the Super Mario Sunshine UHD Texture Pack, and the extras
    python3 tools/mods/get.py extras      only the extras: HD textures the UHD pack lacks
                                          (--if-outdated: only if not the pinned release)
    python3 tools/mods/get.py eclipse     Super Mario Eclipse (patched from your disc), and its maps
    python3 tools/mods/get.py eclipse-maps  only the radar's maps of Eclipse's stages;
                                          --if-outdated as for extras
    python3 tools/mods/get.py eclipse-textures  Super Mario Eclipse's own HD textures (a 9 GB
                                          download, 16 GB installed); --if-outdated as for extras
    python3 tools/mods/get.py achievement-icons  the achievements' icons (Eclipse's too), into assets/achievements/
    python3 tools/mods/get.py language fr the game's text in French (de, es, it, pl: German,
                                          Spanish, Italian, Polish); --if-outdated as for extras
    python3 tools/mods/get.py lua         the lua mod, which runs Lua script mods (native
                                          mods, docs in the mod SDK); --if-outdated as for extras
    python3 tools/mods/get.py all         textures and Eclipse

Each mod's previous install is removed first, then its release is downloaded
from where its authors publish it, checked against a known checksum, and
installed under mods/. None of them is part of this repository.

Options:
    --iso PATH        your Super Mario Sunshine image (North America, the
                      unmodified 1:1 ISO), for Eclipse; by default the one
                      named by SMS_DISC_IMAGE or disc_image in settings.txt,
                      or the one in rom/
    --keep-download   keep the downloaded archives in mods/.downloads/

Needs Python 3 and 7-Zip (7z, 7zz or 7za on PATH).
"""

import argparse
import hashlib
import http.client
import io
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vcdiff  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
MODS = os.path.join(ROOT, "mods")
DOWNLOADS = os.path.join(MODS, ".downloads")

# Pinned releases. To move to a newer one, change the URL and checksums
# together (the log of a failed check prints the new checksum).
TEXTURES = {
    "name": "Super Mario Sunshine UHD Texture Pack v2.1.1 (qashto, razius)",
    "page": "https://github.com/qashto/Super_Mario_Sunshine_UHD_Texture_Pack",
    "url": "https://github.com/qashto/Super_Mario_Sunshine_UHD_Texture_Pack/releases/download/2.1.1/GMS.7z",
    "file": "GMS.7z",
    "md5": "274985dba9214b857b3e5605d2ee49b2",
    "size": 986431331,
    "unpacked": 3218563135,
    "install": os.path.join(MODS, "textures", "GMS"),
}
# HD textures the UHD pack lacks (the boot logo, GAME OVER, the pause guide's
# pictures, HUD icons, Peach, stage textures, ...), made for this port with the
# scripts in that repository. The release is pinned in texture-extras.json,
# which SMS Launcher also reads: it installs the extras again whenever the
# release recorded in the install's .release file is not that one.
with open(os.path.join(HERE, "texture-extras.json"), encoding="utf-8") as f:
    EXTRAS = dict(json.load(f), install=os.path.join(MODS, "textures", "sms-hd-texture-extras"))
EXTRAS_RECORD = ".release"
# The language packs `language = <code>` in settings.txt plays the game in
# (platform/misc/language.cpp): the European release's four and community
# translations, pinned in language-packs.json. "any_language" there tells SMS
# Launcher that this game plays any installed pack, so the launcher can offer
# languages released after it.
with open(os.path.join(HERE, "language-packs.json"), encoding="utf-8") as f:
    LANGUAGE_PACKS = json.load(f)
# The lua mod (https://github.com/chasem-dev/sms-mod-sdk, its lua/): a native
# mod that runs Lua script mods (`runtime = lua` in their mod.ini). Optional,
# so not part of the game; its release holds lua/ with a library for every
# platform. Pinned in lua-mod.json, for SMS Launcher too.
with open(os.path.join(HERE, "lua-mod.json"), encoding="utf-8") as f:
    LUA = dict(json.load(f), install=os.path.join(MODS, "lua"))
# The radar's maps of Super Mario Eclipse's stages (src/radar.cpp), made from
# its disc by tools/radar/gen_maps.py --except assets/maps/maps.txt: the
# stages it added or changed. Installed with Eclipse, in mods/maps/eclipse,
# where the game reads them beside assets/maps. Pinned in eclipse-maps.json,
# for SMS Launcher too.
with open(os.path.join(HERE, "eclipse-maps.json"), encoding="utf-8") as f:
    ECLIPSE_MAPS = dict(json.load(f), install=os.path.join(MODS, "maps", "eclipse"))
# Super Mario Eclipse's own HD textures: iZePlayzYT's SME HiRes Texture Pack, a
# Dolphin pack for Eclipse's disc (GMSE04), in BC7 DDS files the texture loader
# reads (platform/gx/src/gx_hires.cpp). Pinned in eclipse-textures.json, for
# SMS Launcher too. Installed in mods/eclipse-textures, outside mods/textures,
# so Super Mario Sunshine never loads it: SMS Launcher names that folder in
# SMS_TEXTURE_PACKS, ahead of mods/textures, when it plays Eclipse with HD
# textures, so Eclipse's textures win where both packs replace one.
with open(os.path.join(HERE, "eclipse-textures.json"), encoding="utf-8") as f:
    ECLIPSE_TEXTURES = dict(json.load(f), install=os.path.join(MODS, "eclipse-textures"))
ECLIPSE = {
    "name": "Super Mario Eclipse v1.1.0 (Eclipse Team)",
    "page": "https://gamebanana.com/mods/536309",
    # The GameBanana release, mirrored for SMS Launcher; GameBanana itself is
    # the fallback. Both must match md5.
    "url": "https://sms-eclipse.tekrantgaming.com/files/super_mario_eclipse_v110.7z",
    "fallback_urls": ["https://gamebanana.com/dl/1729332"],
    "file": "super_mario_eclipse_v110.7z",
    "md5": "37c1805ab88b2a3bb2a96bcdf6884433",
    "size": 892257115,
    "patch": "patches/v1.1.0.xdelta",
    "patch_size": 1191516980,
    "source_md5": "0c6d2edae9fdf40dfc410ff1623e4119",  # GMSE01, as Eclipse's own patcher requires
    "result_md5": "caa546309e0443f7b47632b040a250d7",
    "install": os.path.join(MODS, "eclipse"),
    "iso": "Super Mario Eclipse v1.1.0.iso",
}


class Failure(Exception):
    pass


def say(msg):
    print(msg, flush=True)


def mib(n):
    return "%.0f MiB" % (n / 1048576.0)


def remove(path):
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    elif os.path.lexists(path):
        os.remove(path)


def need_space(path, nbytes):
    os.makedirs(path, exist_ok=True)
    free = shutil.disk_usage(path).free
    if free < nbytes:
        raise Failure("needs %s free under %s, and %s is" % (mib(nbytes), path, mib(free)))


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# A download that stops short (a dropped connection, or the installer stopped)
# is deleted, never kept: the next attempt starts it over.
def download(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": "sms-pc-port mod installer"})
    part = dest + ".part"
    try:
        with urllib.request.urlopen(req) as r, open(part, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            done, last = 0, 0.0
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                now = time.time()
                if now - last > 1.0 and sys.stdout.isatty():
                    last = now
                    pct = " (%d%%)" % (100 * done // total) if total else ""
                    print("\r  %s%s   " % (mib(done), pct), end="", flush=True)
        if sys.stdout.isatty():
            print("\r" + " " * 30 + "\r", end="")
        if total and done != total:
            raise OSError("the download stopped at %s of %s" % (mib(done), mib(total)))
        os.replace(part, dest)
    except BaseException:
        remove(part)
        raise


# Downloads mod["url"], or each of mod["fallback_urls"] in turn when one
# fails twice or sends a file whose MD5 is not the release's.
def fetch(mod):
    os.makedirs(DOWNLOADS, exist_ok=True)
    dest = os.path.join(DOWNLOADS, mod["file"])
    failed, mismatch = [], None
    for url in [mod["url"]] + mod.get("fallback_urls", []):
        remove(dest)
        say("Downloading %s\n  from %s" % (mod["name"], url))
        for attempt in (1, 2):
            try:
                download(url, dest)
                break
            except (OSError, http.client.HTTPException) as e:
                say("  failed: %s%s" % (e, "; starting over" if attempt == 1 else ""))
                error = e
        else:
            failed.append("%s: %s" % (url, error))
            continue
        got = md5_of(dest)
        if mod["md5"] and got != mod["md5"]:
            remove(dest)
            say("  MD5 %s, expected %s" % (got, mod["md5"]))
            mismatch = "MD5 %s, expected %s" % (got, mod["md5"])
            failed.append("%s: %s" % (url, mismatch))
            continue
        say("  %s, MD5 %s" % (mib(os.path.getsize(dest)), got))
        return dest
    if mismatch and len(failed) == 1:
        raise Failure("the download does not match the expected release (%s); see %s" % (mismatch, mod["page"]))
    if mismatch:
        raise Failure("no download matched the expected release (%s); see %s" % ("; ".join(failed), mod["page"]))
    raise Failure("download failed: %s" % "; ".join(failed))


def seven_zip():
    for name in ("7z", "7zz", "7za"):
        exe = shutil.which(name)
        if exe:
            return exe
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)")):
        if base and os.path.isfile(os.path.join(base, "7-Zip", "7z.exe")):
            return os.path.join(base, "7-Zip", "7z.exe")
    raise Failure("7-Zip is needed to unpack the download: install it (Linux: the 7zip or"
                  " p7zip-full package; macOS: brew install sevenzip; Windows: 7-zip.org)")


def extract(archive, dest, members=()):
    remove(dest)
    os.makedirs(dest)
    say("Unpacking %s" % os.path.basename(archive))
    r = subprocess.run([seven_zip(), "x", "-y", "-o" + dest, archive] + list(members),
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        raise Failure("7-Zip could not unpack %s:\n%s" % (archive, r.stderr.strip()))


def settings_disc():
    for path in (os.path.join(ROOT, "settings.txt"),):
        try:
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("#") or "=" not in line:
                        continue
                    k, v = (s.strip() for s in line.split("=", 1))
                    if k in ("disc_image", "SMS_DISC_IMAGE") and v:
                        return v
        except OSError:
            pass
    return None


def find_iso(explicit):
    if explicit:
        return explicit
    for cand in (os.environ.get("SMS_DISC_IMAGE"), settings_disc()):
        if cand:
            return cand
    rom = os.path.join(ROOT, "rom")
    if os.path.isdir(rom):
        images = sorted(n for n in os.listdir(rom) if n.lower().endswith((".iso", ".gcm")))
        if len(images) == 1:
            return os.path.join(rom, images[0])
        if len(images) > 1:
            raise Failure("rom/ holds several images; name yours with --iso")
    raise Failure("no Super Mario Sunshine image found: pass --iso PATH (or put it in rom/)")


def get_textures(keep):
    mod = TEXTURES
    say("== Textures: %s" % mod["name"])
    seven_zip()  # before downloading anything
    if os.path.isdir(mod["install"]):
        say("Removing the previous install, %s" % os.path.relpath(mod["install"], ROOT))
        remove(mod["install"])
    need_space(MODS, mod["size"] + mod["unpacked"] + (64 << 20))
    archive = fetch(mod)
    tmp = os.path.join(DOWNLOADS, "textures")
    try:
        extract(archive, tmp)
        # the release holds GMS/Textures/GMS/<folders of tex1_* files>
        src = os.path.join(tmp, "GMS", "Textures", "GMS")
        if not os.path.isdir(src):
            raise Failure("the archive is not laid out as expected (no GMS/Textures/GMS)")
        os.makedirs(os.path.dirname(mod["install"]), exist_ok=True)
        shutil.move(src, mod["install"])
        count = sum(1 for _, _, files in os.walk(mod["install"])
                    for n in files if n.startswith("tex1_"))
    finally:
        remove(tmp)
        if not keep:
            remove(archive)
    say("Installed %d textures in %s." % (count, os.path.relpath(mod["install"], ROOT)))
    try:
        get_extras(keep)
    except Failure as e:
        say("warning: the UHD pack is installed, but not the extras (%s);"
            " python3 tools/mods/get.py extras tries again" % e)
    say("The port uses them on its next start; see mods/README.md for the memory budget"
        " and SMS_GX_SCALE for a higher internal resolution.")


def extras_current():
    """Whether the installed extras are the pinned release."""
    try:
        with open(os.path.join(EXTRAS["install"], EXTRAS_RECORD), encoding="utf-8") as f:
            return json.load(f).get("md5") == EXTRAS["md5"]
    except (OSError, ValueError, AttributeError):
        return False


def get_extras(keep, if_outdated=False):
    mod = EXTRAS
    if if_outdated and extras_current():
        say("The texture extras are up to date (%s)." % mod["name"])
        return
    say("== Texture extras: %s" % mod["name"])
    if os.path.isdir(mod["install"]):
        say("Removing the previous install, %s" % os.path.relpath(mod["install"], ROOT))
        remove(mod["install"])
    need_space(MODS, 2 * mod["size"] + (16 << 20))
    archive = fetch(mod)
    tmp = mod["install"] + ".part"
    remove(tmp)
    try:
        # the zip holds GMS/<folders of tex1_* files>, as Dolphin's Load/Textures/ takes it
        with zipfile.ZipFile(archive) as z:
            names = [n for n in z.namelist() if not n.endswith("/")]
            if not names or any(not n.startswith("GMS/") or ".." in n.split("/") for n in names):
                raise Failure("the archive is not laid out as expected (GMS/...)")
            z.extractall(tmp)
        os.replace(os.path.join(tmp, "GMS"), mod["install"])
        count = sum(1 for _, _, files in os.walk(mod["install"])
                    for n in files if n.startswith("tex1_"))
        with open(os.path.join(mod["install"], EXTRAS_RECORD), "w", encoding="utf-8") as f:
            json.dump({"name": mod["name"], "file": mod["file"], "md5": mod["md5"]}, f, indent=2)
    except (OSError, zipfile.BadZipFile) as e:
        remove(mod["install"])
        raise Failure("could not unpack %s: %s" % (os.path.basename(archive), e))
    finally:
        remove(tmp)
        if not keep:
            remove(archive)
    say("Installed %d textures in %s." % (count, os.path.relpath(mod["install"], ROOT)))


def language_install(code):
    return os.path.join(MODS, "languages", code)


def language_current(code):
    """Whether the installed pack for `code` is the pinned release."""
    try:
        with open(os.path.join(language_install(code), EXTRAS_RECORD), encoding="utf-8") as f:
            return json.load(f).get("md5") == LANGUAGE_PACKS["languages"][code]["md5"]
    except (OSError, ValueError, AttributeError, KeyError):
        return False


def get_language(code, keep, if_outdated=False):
    langs = LANGUAGE_PACKS["languages"]
    if code not in langs:
        raise Failure("no language pack %s (there are %s)" % (code, ", ".join(sorted(langs))))
    mod = dict(langs[code], page=LANGUAGE_PACKS["page"],
               name="%s (%s)" % (LANGUAGE_PACKS["name"], langs[code]["name"]))
    install = language_install(code)
    if if_outdated and language_current(code):
        say("The %s language pack is up to date (%s)." % (langs[code]["name"], LANGUAGE_PACKS["name"]))
        return
    say("== Language pack: %s" % mod["name"])
    if os.path.isdir(install):
        say("Removing the previous install, %s" % os.path.relpath(install, ROOT))
        remove(install)
    need_space(MODS, 3 * mod["size"] + (16 << 20))
    archive = fetch(mod)
    tmp = install + ".part"
    remove(tmp)
    try:
        # the zip holds <code>/files/..., as the port reads mods/languages/<code>/files/
        with zipfile.ZipFile(archive) as z:
            names = [n for n in z.namelist() if not n.endswith("/")]
            if not names or any(not n.startswith(code + "/files/") or ".." in n.split("/") for n in names):
                raise Failure("the archive is not laid out as expected (%s/files/...)" % code)
            z.extractall(tmp)
        os.makedirs(os.path.dirname(install), exist_ok=True)
        os.replace(os.path.join(tmp, code), install)
        with open(os.path.join(install, EXTRAS_RECORD), "w", encoding="utf-8") as f:
            json.dump({"name": mod["name"], "file": mod["file"], "md5": mod["md5"]}, f, indent=2)
    except (OSError, zipfile.BadZipFile) as e:
        remove(install)
        raise Failure("could not unpack %s: %s" % (os.path.basename(archive), e))
    finally:
        remove(tmp)
        if not keep:
            remove(archive)
    say("Installed %s in %s; `language = %s` in settings.txt plays the game in it."
        % (langs[code]["name"], os.path.relpath(install, ROOT), code))


def lua_current():
    """Whether the installed lua mod is the pinned release."""
    try:
        with open(os.path.join(LUA["install"], EXTRAS_RECORD), encoding="utf-8") as f:
            return json.load(f).get("md5") == LUA["md5"]
    except (OSError, ValueError, AttributeError):
        return False


def get_lua(keep, if_outdated=False):
    mod = LUA
    if if_outdated and lua_current():
        say("The lua mod is up to date (%s)." % mod["name"])
        return
    say("== The lua mod: %s" % mod["name"])
    if os.path.isdir(mod["install"]):
        say("Removing the previous install, %s" % os.path.relpath(mod["install"], ROOT))
        remove(mod["install"])
    need_space(MODS, 3 * mod["size"] + (16 << 20))
    archive = fetch(mod)
    tmp = mod["install"] + ".part"
    remove(tmp)
    try:
        # the zip holds lua/ (mod.ini, bin/<platform>/lua_host.*), as mods/lua/
        with zipfile.ZipFile(archive) as z:
            names = [n for n in z.namelist() if not n.endswith("/")]
            if "lua/mod.ini" not in names or any(not n.startswith("lua/") or ".." in n.split("/") for n in names):
                raise Failure("the archive is not laid out as expected (lua/mod.ini, ...)")
            z.extractall(tmp)
        os.replace(os.path.join(tmp, "lua"), mod["install"])
        with open(os.path.join(mod["install"], EXTRAS_RECORD), "w", encoding="utf-8") as f:
            json.dump({"name": mod["name"], "file": mod["file"], "md5": mod["md5"]}, f, indent=2)
    except (OSError, zipfile.BadZipFile) as e:
        remove(mod["install"])
        raise Failure("could not unpack %s: %s" % (os.path.basename(archive), e))
    finally:
        remove(tmp)
        if not keep:
            remove(archive)
    platforms = sorted(os.listdir(os.path.join(mod["install"], "bin")))
    say("Installed the lua mod in %s (%s); Lua script mods in mods/ now run."
        % (os.path.relpath(mod["install"], ROOT), ", ".join(platforms)))


def eclipse_maps_current():
    """Whether the installed Eclipse maps are the pinned release."""
    try:
        with open(os.path.join(ECLIPSE_MAPS["install"], EXTRAS_RECORD), encoding="utf-8") as f:
            return json.load(f).get("md5") == ECLIPSE_MAPS["md5"]
    except (OSError, ValueError, AttributeError):
        return False


def get_eclipse_maps(keep, if_outdated=False):
    mod = ECLIPSE_MAPS
    if if_outdated and eclipse_maps_current():
        say("The Eclipse maps are up to date (%s)." % mod["name"])
        return
    say("== Eclipse maps: %s" % mod["name"])
    if os.path.isdir(mod["install"]):
        say("Removing the previous install, %s" % os.path.relpath(mod["install"], ROOT))
        remove(mod["install"])
    need_space(MODS, 2 * mod["size"] + (16 << 20))
    archive = fetch(mod)
    tmp = mod["install"] + ".part"
    remove(tmp)
    try:
        # the zip holds eclipse/ (maps.txt and the pictures), as mods/maps/eclipse/
        with zipfile.ZipFile(archive) as z:
            names = [n for n in z.namelist() if not n.endswith("/")]
            if "eclipse/maps.txt" not in names or any(not n.startswith("eclipse/") or ".." in n.split("/")
                                                       for n in names):
                raise Failure("the archive is not laid out as expected (eclipse/maps.txt, ...)")
            z.extractall(tmp)
        os.makedirs(os.path.dirname(mod["install"]), exist_ok=True)
        os.replace(os.path.join(tmp, "eclipse"), mod["install"])
        count = sum(1 for n in os.listdir(mod["install"]) if n.endswith(".jpg"))
        with open(os.path.join(mod["install"], EXTRAS_RECORD), "w", encoding="utf-8") as f:
            json.dump({"name": mod["name"], "file": mod["file"], "md5": mod["md5"]}, f, indent=2)
    except (OSError, zipfile.BadZipFile) as e:
        remove(mod["install"])
        raise Failure("could not unpack %s: %s" % (os.path.basename(archive), e))
    finally:
        remove(tmp)
        if not keep:
            remove(archive)
    say("Installed %d maps in %s; the radar shows them in Eclipse's stages."
        % (count, os.path.relpath(mod["install"], ROOT)))


# The Eclipse pack is a 9 GB zip of 16 GB of textures. Downloading the zip and
# then unpacking it would need 25 GB at once, so the textures are unpacked as
# they arrive instead: the zip's index (its central directory, which holds every
# texture's size and CRC-32) is read with HTTP range requests and checked
# against the pinned index_md5, then the textures are fetched in runs of about
# RUN bytes, each unpacked and checked against its CRC-32 as it comes. They go
# to mods/eclipse-textures.part/, where a stopped install's finished textures
# are listed (.done) for the next one to carry on from.
RUN = 64 << 20
RUN_TRIES = 5


def http_range(url, start, end):
    """An open response with bytes start..end (inclusive) of url, and the file's size."""
    req = urllib.request.Request(url, headers={"User-Agent": "sms-pc-port mod installer",
                                               "Range": "bytes=%d-%d" % (start, end)})
    r = urllib.request.urlopen(req)
    span = (r.headers.get("Content-Range") or "").split("/")
    if r.status != 206 or len(span) != 2 or not span[0].startswith("bytes %d-" % start):
        r.close()
        raise Failure("%s does not send parts of the file (an HTTP range request), which this"
                      " download needs" % url.split("?")[0])
    return r, int(span[1]) if span[1].isdigit() else None


class RemoteFile(io.RawIOBase):
    """A file on a server, read with range requests, for zipfile to read the index."""

    def __init__(self, url, size):
        self.url, self.size, self.pos = url, size, 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=0):
        self.pos = offset if whence == 0 else self.pos + offset if whence == 1 else self.size + offset
        return self.pos

    def readinto(self, b):
        n = min(len(b), self.size - self.pos)
        if n <= 0:
            return 0
        r, _ = http_range(self.url, self.pos, self.pos + n - 1)
        with r:
            data = r.read(n)
        b[:len(data)] = data
        self.pos += len(data)
        return len(data)


def eclipse_textures_index(url, mod):
    """The pack's textures (ZipInfo, in the zip's order) and where its index starts, checked
    against the pin."""
    r, size = http_range(url, 0, 0)
    r.close()
    if size != mod["size"]:
        raise Failure("%s is %s bytes, not the pinned release's %d" % (url.split("?")[0], size, mod["size"]))
    with zipfile.ZipFile(io.BufferedReader(RemoteFile(url, size), 1 << 20)) as z:
        infos, start = z.infolist(), z.start_dir
    r, _ = http_range(url, start, size - 1)
    with r:
        index = r.read()
    got = hashlib.md5(index).hexdigest()
    if len(index) != size - start or got != mod["index_md5"]:
        raise Failure("the index of %s does not match the pinned release (MD5 %s, expected %s); see %s"
                      % (mod["file"], got, mod["index_md5"], mod["page"]))
    textures = []
    for i in sorted(infos, key=lambda i: i.header_offset):
        parts = i.filename.split("/")
        if i.is_dir() or parts[0] != "GMSE04" or not parts[-1].lower().endswith((".dds", ".png")):
            continue
        if ".." in parts or "" in parts or "\\" in i.filename or i.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            raise Failure("the archive is not laid out as expected (%s)" % i.filename)
        textures.append(i)
    if not textures:
        raise Failure("the archive is not laid out as expected (no GMSE04/ textures)")
    # Each texture's data ends where the next entry starts (a data descriptor
    # may follow it), and the last one's where the index starts.
    offsets = sorted([i.header_offset for i in infos] + [start])
    ends = {o: offsets[k + 1] for k, o in enumerate(offsets[:-1])}
    return textures, ends


def eclipse_textures_current():
    """Whether the installed Eclipse textures are the pinned release."""
    try:
        with open(os.path.join(ECLIPSE_TEXTURES["install"], EXTRAS_RECORD), encoding="utf-8") as f:
            return json.load(f).get("md5") == ECLIPSE_TEXTURES["md5"]
    except (OSError, ValueError, AttributeError):
        return False


def unpack_run(url, run, ends, staging, done, report):
    """Fetches the textures in run (consecutive in the zip) with one range request and
    unpacks each into staging, checking its CRC-32; done records each one finished."""
    first, last = run[0], run[-1]
    r, _ = http_range(url, first.header_offset, ends[last.header_offset] - 1)
    with r:
        pos = first.header_offset
        for i in run:
            def read(n):
                data = r.read(n)
                if len(data) != n:
                    raise OSError("the connection closed in %s" % i.filename)
                return data
            if pos != i.header_offset:
                read(i.header_offset - pos)
            head = read(30)
            if head[:4] != b"PK\x03\x04":
                raise Failure("the archive is not laid out as expected (%s)" % i.filename)
            skip = int.from_bytes(head[26:28], "little") + int.from_bytes(head[28:30], "little")
            read(skip)
            dest = os.path.join(staging, *i.filename.split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            inflate = zlib.decompressobj(-15) if i.compress_type == zipfile.ZIP_DEFLATED else None
            crc, size, left = 0, 0, i.compress_size
            with open(dest + ".tmp", "wb") as out:
                while left:
                    chunk = read(min(left, 1 << 20))
                    left -= len(chunk)
                    report(len(chunk))
                    data = inflate.decompress(chunk) if inflate else chunk
                    out.write(data)
                    crc, size = zlib.crc32(data, crc), size + len(data)
                if inflate:
                    data = inflate.flush()
                    out.write(data)
                    crc, size = zlib.crc32(data, crc), size + len(data)
            if crc != i.CRC or size != i.file_size:
                remove(dest + ".tmp")
                raise OSError("%s arrived damaged (CRC-32 %08x, expected %08x)" % (i.filename, crc, i.CRC))
            os.replace(dest + ".tmp", dest)
            done.write("%s\t%d\n" % (i.filename, i.file_size))
            done.flush()
            pos = i.header_offset + 30 + skip + i.compress_size


def get_eclipse_textures(if_outdated=False):
    mod = ECLIPSE_TEXTURES
    if if_outdated and eclipse_textures_current():
        say("The Eclipse HD textures are up to date (%s)." % mod["name"])
        return
    say("== Eclipse HD textures: %s" % mod["name"])
    staging = mod["install"] + ".part"
    record = os.path.join(staging, ".done")
    # What an earlier install of this release finished; another release's goes.
    finished = {}
    try:
        with open(record, encoding="utf-8") as f:
            if f.readline().strip() == mod["md5"]:
                for line in f:
                    name, _, size = line.rstrip("\n").partition("\t")
                    path = os.path.join(staging, *name.split("/"))
                    if size.isdigit() and os.path.isfile(path) and os.path.getsize(path) == int(size):
                        finished[name] = int(size)
    except OSError:
        pass
    if not finished:
        remove(staging)
    if os.path.isdir(mod["install"]):
        say("Removing the previous install, %s" % os.path.relpath(mod["install"], ROOT))
        remove(mod["install"])
    failed = []
    for url in [mod["url"]] + mod.get("fallback_urls", []):
        say("Downloading %s\n  from %s" % (mod["name"], url.split("?")[0]))
        try:
            textures, ends = eclipse_textures_index(url, mod)
        except (OSError, http.client.HTTPException, zipfile.BadZipFile) as e:
            failed.append("%s: %s" % (url.split("?")[0], e))
            say("  failed: %s" % e)
            continue
        left = [i for i in textures if finished.get(i.filename) != i.file_size]
        total = sum(i.compress_size for i in textures)
        got = [total - sum(i.compress_size for i in left)]
        if finished:
            say("  carrying on: %d of %d textures were installed before" % (len(textures) - len(left), len(textures)))
        need_space(MODS, sum(i.file_size for i in left) + (64 << 20))
        last = [0.0]

        def report(n, force=False):
            got[0] += n
            now = time.monotonic()
            if force or now - last[0] >= 0.5:
                last[0] = now
                print("Eclipse texture download: %d/%d bytes (%d/%d textures)"
                      % (got[0], total, len(textures) - len(left), len(textures)), flush=True)

        os.makedirs(staging, exist_ok=True)
        position = {i.filename: k for k, i in enumerate(textures)}
        with open(record, "a" if finished else "w", encoding="utf-8") as done:
            if not finished:
                done.write(mod["md5"] + "\n")
                done.flush()
            report(0, force=True)
            tries = 0
            while left and tries < RUN_TRIES:
                # A run: textures consecutive in the zip, up to RUN bytes.
                run, span = [left[0]], left[0].compress_size
                while len(run) < len(left) and span < RUN and \
                        position[left[len(run)].filename] == position[run[-1].filename] + 1:
                    span += left[len(run)].compress_size
                    run.append(left[len(run)])
                before = got[0]
                try:
                    unpack_run(url, run, ends, staging, done, report)
                    error = None
                except (OSError, http.client.HTTPException) as e:
                    error = e
                # The textures it finished stay finished, a stopped run's too.
                with open(record, encoding="utf-8") as f:
                    kept = {line.split("\t")[0] for line in f.read().splitlines()[1:]}
                got[0] = before + sum(i.compress_size for i in run if i.filename in kept)
                remaining = [i for i in left if i.filename not in kept]
                tries = 0 if len(remaining) < len(left) else tries + 1
                left = remaining
                if error is not None and left and tries < RUN_TRIES:
                    say("  the download stopped (%s); trying again" % error)
                    time.sleep(min(30, 2 ** tries))
                report(0, force=True)
        if left:
            failed.append("%s: the download kept stopping" % url.split("?")[0])
            finished = {i.filename: i.file_size for i in textures if i.filename not in {j.filename for j in left}}
            continue
        report(0, force=True)
        remove(record)
        with open(os.path.join(staging, EXTRAS_RECORD), "w", encoding="utf-8") as f:
            json.dump({"name": mod["name"], "file": mod["file"], "md5": mod["md5"]}, f, indent=2)
        os.replace(staging, mod["install"])
        say("Installed %d textures in %s; Super Mario Eclipse shows them with HD textures on."
            % (len(textures), os.path.relpath(mod["install"], ROOT)))
        return
    raise Failure("download failed: %s; what was installed is kept for the next try" % "; ".join(failed))


def get_eclipse(keep, iso_arg):
    mod = ECLIPSE
    say("== Eclipse: %s" % mod["name"])
    seven_zip()
    iso = find_iso(iso_arg)
    if not os.path.isfile(iso):
        raise Failure("no such image: %s" % iso)
    say("Checking your image, %s" % iso)
    got = md5_of(iso)
    if got != mod["source_md5"]:
        raise Failure("Eclipse patches only the North American ISO with MD5 %s, and this one is %s"
                      " (compressed or trimmed images such as CISO, NKit and RVZ do not work)"
                      % (mod["source_md5"], got))
    if os.path.isdir(mod["install"]):
        say("Removing the previous install, %s" % os.path.relpath(mod["install"], ROOT))
        remove(mod["install"])
    need_space(MODS, mod["size"] + mod["patch_size"] + os.path.getsize(iso) + (64 << 20))
    archive = fetch(mod)
    tmp = os.path.join(DOWNLOADS, "eclipse")
    out = os.path.join(mod["install"], mod["iso"])
    try:
        extract(archive, tmp, [mod["patch"], "readme.md", "changelog.md", "license.md"])
        if not keep:
            remove(archive)
        os.makedirs(mod["install"])
        say("Patching your image (a minute or two)")
        last = [0.0]

        def progress(done, total):
            now = time.time()
            if sys.stdout.isatty() and now - last[0] > 1.0:
                last[0] = now
                print("\r  %d%%   " % (100 * done // total), end="", flush=True)
            elif not sys.stdout.isatty() and now - last[0] > 5.0:
                # A line now and then, so a log (SMS Launcher's) shows it is working.
                last[0] = now
                say("Patching your image: %d%%" % (100 * done // total))

        try:
            vcdiff.apply(iso, os.path.join(tmp, mod["patch"]), out + ".part", progress)
        except vcdiff.VcdiffError as e:
            raise Failure("patching failed: %s" % e)
        if sys.stdout.isatty():
            print("\r        \r", end="")
        got = md5_of(out + ".part")
        if got != mod["result_md5"]:
            raise Failure("the patched image is not the expected one (MD5 %s, expected %s)"
                          % (got, mod["result_md5"]))
        os.replace(out + ".part", out)
        shutil.copy(os.path.join(tmp, "changelog.md"), os.path.join(mod["install"], "changelog.md"))
    except BaseException:
        remove(mod["install"])
        raise
    finally:
        remove(tmp)
        if not keep:
            remove(archive)
    say("Installed %s." % os.path.relpath(out, ROOT))
    say("It is the same image Eclipse's own patcher makes, for Dolphin or a console. The port"
        " plays it with Eclipse's code built in: SMS_ECLIPSE=1 ./build.sh (docs/ECLIPSE.md).")
    try:
        get_eclipse_maps(keep)
    except Failure as e:
        say("warning: Eclipse is installed, but not the radar's maps of its stages (%s);"
            " python3 tools/mods/get.py eclipse-maps tries again" % e)


# The achievements' icons (ext/achievements): published with the texture
# extras, pinned in achievement-icons.json, installed in assets/achievements/
# (1.png to 148.png) whatever the texture settings, with the release recorded
# in its .release file as for the extras.
ACHIEVEMENT_ICONS = None
if os.path.exists(os.path.join(HERE, "achievement-icons.json")):
    with open(os.path.join(HERE, "achievement-icons.json"), encoding="utf-8") as f:
        ACHIEVEMENT_ICONS = dict(json.load(f), install=os.path.join(ROOT, "assets", "achievements"))


def achievement_icons_current():
    """Whether the installed achievement icons are the pinned release."""
    try:
        with open(os.path.join(ACHIEVEMENT_ICONS["install"], EXTRAS_RECORD), encoding="utf-8") as f:
            return json.load(f).get("md5") == ACHIEVEMENT_ICONS["md5"]
    except (OSError, ValueError, AttributeError, TypeError):
        return False


def get_achievement_icons(keep, if_outdated=False):
    mod = ACHIEVEMENT_ICONS
    if not mod:
        raise Failure("this version of the game has no achievement icons to install")
    if if_outdated and achievement_icons_current():
        say("The achievement icons are up to date (%s)." % mod["name"])
        return
    say("== Achievement icons: %s" % mod["name"])
    need_space(MODS, 2 * mod["size"] + (16 << 20))
    archive = fetch(mod)
    tmp = mod["install"] + ".part"
    remove(tmp)
    try:
        # the zip holds achievements/ (1.png to 148.png, and Super Mario
        # Eclipse's eclipse/1.png to 128.png), as assets/achievements/
        with zipfile.ZipFile(archive) as z:
            names = [n for n in z.namelist() if not n.endswith("/")]
            sunshine = ["achievements/%d.png" % i for i in range(1, 149)]
            eclipse = ["achievements/eclipse/%d.png" % i for i in range(1, 129)]
            if not set(sunshine + eclipse) <= set(names) or any(not n.startswith("achievements/") or ".." in n.split("/") for n in names):
                raise Failure("the archive is not laid out as expected (achievements/1.png to 148.png, achievements/eclipse/1.png to 128.png)")
            z.extractall(tmp)
        if os.path.isdir(mod["install"]):
            remove(mod["install"])
        os.replace(os.path.join(tmp, "achievements"), mod["install"])
        with open(os.path.join(mod["install"], EXTRAS_RECORD), "w", encoding="utf-8") as f:
            json.dump({"name": mod["name"], "file": mod["file"], "md5": mod["md5"]}, f, indent=2)
    except (OSError, zipfile.BadZipFile) as e:
        raise Failure("could not unpack %s: %s" % (os.path.basename(archive), e))
    finally:
        remove(tmp)
        if not keep:
            remove(archive)
    say("Installed the achievement icons in %s." % os.path.relpath(mod["install"], ROOT))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n\n".join(__doc__.split("\n\n")[1:]))
    ap.add_argument("what", choices=["textures", "extras", "eclipse", "eclipse-maps", "eclipse-textures",
                                      "achievement-icons", "language", "lua", "all"])
    ap.add_argument("language", nargs="?", help="with language: fr, de, es, it or pl")
    ap.add_argument("--iso")
    ap.add_argument("--keep-download", action="store_true")
    ap.add_argument("--if-outdated", action="store_true")
    args = ap.parse_args()
    # A download left unfinished by an earlier run (stopped, or the computer
    # turned off) is only in the way, and takes space need_space counts.
    if os.path.isdir(DOWNLOADS):
        for name in os.listdir(DOWNLOADS):
            if name.endswith(".part"):
                remove(os.path.join(DOWNLOADS, name))
    try:
        if args.what in ("textures", "all"):
            get_textures(args.keep_download)
        if args.what == "extras":
            get_extras(args.keep_download, args.if_outdated)
        if args.what == "language":
            if not args.language:
                raise Failure("which language? fr, de, es, it or pl")
            get_language(args.language, args.keep_download, args.if_outdated)
        if args.what == "lua":
            get_lua(args.keep_download, args.if_outdated)
        if args.what == "eclipse-maps":
            get_eclipse_maps(args.keep_download, args.if_outdated)
        if args.what == "eclipse-textures":
            get_eclipse_textures(args.if_outdated)
        if args.what == "achievement-icons":
            get_achievement_icons(args.keep_download, args.if_outdated)
        if args.what in ("eclipse", "all"):
            get_eclipse(args.keep_download, args.iso)
    except Failure as e:
        sys.exit("error: %s" % e)
    except KeyboardInterrupt:
        sys.exit("stopped")
    finally:
        try:
            os.rmdir(DOWNLOADS)  # only when empty
        except OSError:
            pass


if __name__ == "__main__":
    main()
